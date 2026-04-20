# TLS 加密与认证使用指南

本文档介绍如何在 etcd MVP 中启用 TLS 加密通信和基础认证机制。

## TLS 加密通信

### 生成证书

使用提供的脚本生成自签名证书:

```bash
bash scripts/generate_certs.sh ./certs
```

这将生成以下文件:
- `ca-cert.pem`: CA 证书
- `server-cert.pem`: 服务器证书
- `server-key.pem`: 服务器私钥
- `client-cert.pem`: 客户端证书 (用于 mTLS)
- `client-key.pem`: 客户端私钥 (用于 mTLS)

### 启用 TLS

设置环境变量:

```bash
export ETCD_MVP_TLS_CERT=./certs/server-cert.pem
export ETCD_MVP_TLS_KEY=./certs/server-key.pem
```

启动服务器:

```bash
./etcd_mvp --id 1 --listen 0.0.0.0:2379
```

服务器将使用 TLS 加密通信。

### 启用 mTLS (双向认证)

除了服务器证书外，还需要设置 CA 证书:

```bash
export ETCD_MVP_TLS_CERT=./certs/server-cert.pem
export ETCD_MVP_TLS_KEY=./certs/server-key.pem
export ETCD_MVP_TLS_CA=./certs/ca-cert.pem
```

客户端连接时需要提供客户端证书。

## 基础认证机制

### 启用认证

设置环境变量:

```bash
export ETCD_MVP_AUTH_ENABLED=true
export ETCD_MVP_ROOT_PASSWORD=your_secure_password
```

启动服务器后，系统会自动创建 root 用户。

### 认证流程

#### 1. 获取 Token

```bash
# 使用 root 用户登录
grpcurl -plaintext -d '{
  "username": "root",
  "password": "your_secure_password"
}' localhost:2379 etcdmvp.Auth/Authenticate
```

响应:
```json
{
  "header": {
    "code": "OK"
  },
  "token": "token-root-1234567890-1"
}
```

#### 2. 使用 Token 访问 API

在请求的 metadata 中添加 token:

```bash
grpcurl -plaintext \
  -H "token: token-root-1234567890-1" \
  -d '{"key": "foo", "value": "bar"}' \
  localhost:2379 etcdmvp.KV/Put
```

### 用户管理

#### 添加用户

```bash
grpcurl -plaintext \
  -H "token: <root_token>" \
  -d '{
    "username": "alice",
    "password": "alice_password",
    "token": "<root_token>"
  }' localhost:2379 etcdmvp.Auth/UserAdd
```

#### 删除用户

```bash
grpcurl -plaintext \
  -H "token: <root_token>" \
  -d '{
    "username": "alice",
    "token": "<root_token>"
  }' localhost:2379 etcdmvp.Auth/UserDelete
```

#### 修改密码

```bash
grpcurl -plaintext \
  -H "token: <user_token>" \
  -d '{
    "username": "alice",
    "password": "new_password",
    "token": "<user_token>"
  }' localhost:2379 etcdmvp.Auth/UserChangePassword
```

### 角色与权限管理

#### 创建角色

```bash
grpcurl -plaintext \
  -H "token: <root_token>" \
  -d '{
    "name": "reader",
    "token": "<root_token>"
  }' localhost:2379 etcdmvp.Auth/RoleAdd
```

#### 授予权限

```bash
# 授予读权限
grpcurl -plaintext \
  -H "token: <root_token>" \
  -d '{
    "role": "reader",
    "permission": "READ",
    "key": "/app/",
    "range_end": "/app0",
    "token": "<root_token>"
  }' localhost:2379 etcdmvp.Auth/RoleGrantPermission

# 授予写权限
grpcurl -plaintext \
  -H "token: <root_token>" \
  -d '{
    "role": "writer",
    "permission": "WRITE",
    "key": "/app/",
    "range_end": "/app0",
    "token": "<root_token>"
  }' localhost:2379 etcdmvp.Auth/RoleGrantPermission

# 授予读写权限
grpcurl -plaintext \
  -H "token: <root_token>" \
  -d '{
    "role": "admin",
    "permission": "READWRITE",
    "key": "",
    "range_end": "\xff",
    "token": "<root_token>"
  }' localhost:2379 etcdmvp.Auth/RoleGrantPermission
```

#### 将角色授予用户

```bash
grpcurl -plaintext \
  -H "token: <root_token>" \
  -d '{
    "username": "alice",
    "role": "reader",
    "token": "<root_token>"
  }' localhost:2379 etcdmvp.Auth/UserGrantRole
```

## 权限模型

### 权限类型

- `READ`: 只读权限
- `WRITE`: 只写权限
- `READWRITE`: 读写权限

### Key 范围

- 单个 key: `key="/app/config"`, `range_end=""`
- Key 前缀: `key="/app/"`, `range_end="/app0"` (匹配所有以 `/app/` 开头的 key)
- 所有 key: `key=""`, `range_end="\xff"`

### 权限检查

- 认证未启用时，所有请求都被允许
- root 用户拥有所有权限
- 其他用户需要通过角色获得权限
- 权限检查按 key 范围匹配

## 同时启用 TLS 和认证

```bash
# 设置 TLS
export ETCD_MVP_TLS_CERT=./certs/server-cert.pem
export ETCD_MVP_TLS_KEY=./certs/server-key.pem
export ETCD_MVP_TLS_CA=./certs/ca-cert.pem

# 设置认证
export ETCD_MVP_AUTH_ENABLED=true
export ETCD_MVP_ROOT_PASSWORD=secure_password

# 启动服务器
./etcd_mvp --id 1 --listen 0.0.0.0:2379
```

客户端需要:
1. 使用 TLS 连接
2. 提供客户端证书 (如果启用 mTLS)
3. 在请求中包含有效的 token

## 错误码

- `UNAUTHENTICATED` (9): Token 无效或未提供
- `PERMISSION_DENIED` (10): 用户没有执行操作的权限

## 安全建议

1. **生产环境使用正式 CA 签发的证书**，不要使用自签名证书
2. **使用强密码**，建议至少 16 字符，包含大小写字母、数字和特殊字符
3. **定期轮转证书和密码**
4. **限制 root 用户使用**，为不同应用创建专用用户和角色
5. **使用最小权限原则**，只授予必要的权限
6. **启用 mTLS**，增强客户端身份验证
7. **保护私钥文件**，设置适当的文件权限 (chmod 600)
8. **使用环境变量或密钥管理系统**存储敏感信息，不要硬编码

## 故障排查

### TLS 连接失败

- 检查证书路径是否正确
- 验证证书是否过期: `openssl x509 -in cert.pem -noout -dates`
- 确认 CN 或 SAN 与服务器地址匹配

### 认证失败

- 确认 `ETCD_MVP_AUTH_ENABLED=true` 已设置
- 检查 token 是否有效
- 验证用户是否有相应权限

### 权限被拒绝

- 确认用户已被授予相应角色
- 检查角色的权限范围是否覆盖目标 key
- 验证权限类型 (READ/WRITE/READWRITE) 是否匹配操作
