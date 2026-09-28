package objstore

import (
	"context"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"strings"
	"time"

	"github.com/minio/minio-go/v7"
	"github.com/minio/minio-go/v7/pkg/credentials"
	"gopkg.in/ini.v1"
)

type s3Backend struct {
	client         *minio.Client
	bucket         string
	requestTimeout time.Duration
}

func newS3Backend(section *ini.Section) (*s3Backend, error) {
	required := func(name string) (string, error) {
		value := section.Key(name).String()
		if value == "" {
			return "", fmt.Errorf("[%s] %s is required", section.Name(), name)
		}
		return value, nil
	}
	endpoint := section.Key("endpoint").String()
	if endpoint == "" {
		var err error
		endpoint, err = required("host")
		if err != nil {
			return nil, err
		}
	}
	bucket, err := required("bucket")
	if err != nil {
		return nil, err
	}
	keyID, err := required("key_id")
	if err != nil {
		return nil, err
	}
	key, err := required("key")
	if err != nil {
		return nil, err
	}
	if !section.Key("use_v4_signature").MustBool(true) {
		return nil, fmt.Errorf("[%s] only AWS Signature V4 is supported", section.Name())
	}
	bucketLookup := minio.BucketLookupDNS
	if section.Key("path_style_request").MustBool(true) {
		bucketLookup = minio.BucketLookupPath
	}
	endpoint, secure, err := parseS3Endpoint(endpoint, section.Key("use_https").MustBool(true))
	if err != nil {
		return nil, err
	}
	requestTimeout := time.Duration(section.Key("request_timeout").MustInt(60)) * time.Second
	if requestTimeout <= 0 {
		return nil, fmt.Errorf("[%s] request_timeout must be positive", section.Name())
	}
	connectTimeout := time.Duration(section.Key("connection_timeout").MustInt(10)) * time.Second
	if connectTimeout <= 0 {
		return nil, fmt.Errorf("[%s] connection_timeout must be positive", section.Name())
	}
	maxRetries := section.Key("max_retries").MustInt(2)
	if maxRetries < 0 || maxRetries > 10 {
		return nil, fmt.Errorf("[%s] max_retries must be between 0 and 10", section.Name())
	}
	client, err := minio.New(endpoint, &minio.Options{
		Creds:        credentials.NewStaticV4(keyID, key, ""),
		Secure:       secure,
		Region:       section.Key("aws_region").MustString("us-east-1"),
		BucketLookup: bucketLookup,
		// MinIO counts total attempts, while the CloudFile option counts
		// retries after the first attempt.
		MaxRetries: maxRetries + 1,
		Transport: &http.Transport{
			Proxy:                 http.ProxyFromEnvironment,
			DialContext:           (&net.Dialer{Timeout: connectTimeout, KeepAlive: 30 * time.Second}).DialContext,
			ForceAttemptHTTP2:     true,
			MaxIdleConns:          100,
			MaxIdleConnsPerHost:   10,
			IdleConnTimeout:       90 * time.Second,
			TLSHandshakeTimeout:   connectTimeout,
			ResponseHeaderTimeout: requestTimeout,
			ExpectContinueTimeout: time.Second,
		},
	})
	if err != nil {
		return nil, err
	}
	return &s3Backend{client: client, bucket: bucket, requestTimeout: requestTimeout}, nil
}

func parseS3Endpoint(value string, secure bool) (string, bool, error) {
	if !strings.Contains(value, "://") {
		return value, secure, nil
	}

	u, err := url.Parse(value)
	if err != nil || u.Host == "" || u.Path != "" && u.Path != "/" || u.RawQuery != "" || u.Fragment != "" {
		return "", false, fmt.Errorf("invalid S3 endpoint %q", value)
	}
	switch u.Scheme {
	case "http":
		return u.Host, false, nil
	case "https":
		return u.Host, true, nil
	default:
		return "", false, fmt.Errorf("unsupported S3 endpoint scheme %q", u.Scheme)
	}
}

func (b *s3Backend) objectKey(repoID, objID string) string {
	// S3 stores each object directly below its repository/storage ID. The
	// two-level hash fan-out belongs only to the local filesystem backend.
	return repoID + "/" + objID
}

func (b *s3Backend) read(repoID, objID string, w io.Writer) error {
	ctx, cancel := b.newRequestContext()
	defer cancel()
	object, err := b.client.GetObject(ctx, b.bucket, b.objectKey(repoID, objID), minio.GetObjectOptions{})
	if err != nil {
		return err
	}
	defer object.Close()
	_, err = io.Copy(w, object)
	return err
}

func (b *s3Backend) write(repoID, objID string, r io.Reader, sync bool) error {
	ctx, cancel := b.newRequestContext()
	defer cancel()
	_, err := b.client.PutObject(ctx, b.bucket, b.objectKey(repoID, objID), r, -1, minio.PutObjectOptions{})
	return err
}

func (b *s3Backend) exists(repoID, objID string) (bool, error) {
	ctx, cancel := b.newRequestContext()
	defer cancel()
	_, err := b.client.StatObject(ctx, b.bucket, b.objectKey(repoID, objID), minio.StatObjectOptions{})
	if isS3ObjectNotFound(err) {
		return false, nil
	}
	return err == nil, err
}

func (b *s3Backend) stat(repoID, objID string) (int64, error) {
	ctx, cancel := b.newRequestContext()
	defer cancel()
	info, err := b.client.StatObject(ctx, b.bucket, b.objectKey(repoID, objID), minio.StatObjectOptions{})
	if err != nil {
		return -1, err
	}
	return info.Size, nil
}

func (b *s3Backend) newRequestContext() (context.Context, context.CancelFunc) {
	return context.WithTimeout(context.Background(), b.requestTimeout)
}

func isS3ObjectNotFound(err error) bool {
	if err == nil {
		return false
	}
	switch minio.ToErrorResponse(err).Code {
	case "NoSuchKey", "NoSuchObject", "NoSuchVersion":
		return true
	default:
		return false
	}
}
