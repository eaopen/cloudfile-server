package objstore

import (
	"context"
	"fmt"
	"io"
	"strings"

	"github.com/minio/minio-go/v7"
	"github.com/minio/minio-go/v7/pkg/credentials"
	"gopkg.in/ini.v1"
)

type s3Backend struct {
	client *minio.Client
	bucket string
}

func newS3Backend(section *ini.Section) (*s3Backend, error) {
	required := func(name string) (string, error) {
		value := section.Key(name).String()
		if value == "" {
			return "", fmt.Errorf("[%s] %s is required", section.Name(), name)
		}
		return value, nil
	}
	endpoint, err := required("host")
	if err != nil {
		return nil, err
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
	client, err := minio.New(strings.TrimPrefix(strings.TrimPrefix(endpoint, "https://"), "http://"), &minio.Options{
		Creds:        credentials.NewStaticV4(keyID, key, ""),
		Secure:       section.Key("use_https").MustBool(true),
		Region:       section.Key("aws_region").MustString("us-east-1"),
		BucketLookup: bucketLookup,
	})
	if err != nil {
		return nil, err
	}
	return &s3Backend{client: client, bucket: bucket}, nil
}

func (b *s3Backend) objectKey(repoID, objID string) string {
	return repoID + "/" + objID[:2] + "/" + objID[2:]
}

func (b *s3Backend) read(repoID, objID string, w io.Writer) error {
	object, err := b.client.GetObject(context.Background(), b.bucket, b.objectKey(repoID, objID), minio.GetObjectOptions{})
	if err != nil {
		return err
	}
	defer object.Close()
	_, err = io.Copy(w, object)
	return err
}

func (b *s3Backend) write(repoID, objID string, r io.Reader, sync bool) error {
	_, err := b.client.PutObject(context.Background(), b.bucket, b.objectKey(repoID, objID), r, -1, minio.PutObjectOptions{})
	return err
}

func (b *s3Backend) exists(repoID, objID string) (bool, error) {
	_, err := b.client.StatObject(context.Background(), b.bucket, b.objectKey(repoID, objID), minio.StatObjectOptions{})
	if minio.ToErrorResponse(err).Code == "NoSuchKey" {
		return false, nil
	}
	return err == nil, err
}

func (b *s3Backend) stat(repoID, objID string) (int64, error) {
	info, err := b.client.StatObject(context.Background(), b.bucket, b.objectKey(repoID, objID), minio.StatObjectOptions{})
	if err != nil {
		return -1, err
	}
	return info.Size, nil
}
