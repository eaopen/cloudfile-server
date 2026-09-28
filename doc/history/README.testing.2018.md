<!-- generated-by: gsd-doc-writer -->
# Seafile Server Tests（2018 归档）

> 用途：保留上游 2018 年测试说明原文。
> 适用版本：历史 Seafile Server；不适用于当前 CloudFile CE 14 扩展分支。
> 状态：暂停（历史归档）。

## Run it locally

To run the tests, you need to install pytest first:

```sh
pip install -r ci/requirements.txt
```

Compile and install ccnet-server and seafile-server

```
cd ccnet-server
make
sudo make install

cd seafile-server
make
sudo make install
```

Then run the tests with

```sh
cd seafile-server
./run_tests.sh
```

By default the test script would try to start ccnet-server and seaf-server in `/usr/local/bin`, if you `make install` to another location, say `/opt/local`, run it like this:

```sh
SEAFILE_INSTALL_PREFIX=/opt/local ./run_tests.sh
```
