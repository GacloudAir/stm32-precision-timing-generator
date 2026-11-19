# 仓库维护指南 / Maintenance Guide

本仓库在 **GitHub** 和 **Gitea（Forgejo）** 双端托管，两端内容需保持一致。
本文件面向所有协作者，说明如何克隆、认证、推送双端，以及常见故障的处理。

---

## 一、仓库地址

| 远端名 | 平台 | 地址 | 用途 |
| --- | --- | --- | --- |
| `origin` | GitHub（公网主源） | `https://github.com/GacloudAir/stm32-precision-timing-generator.git` | 对外开源主仓库 |
| `gitea` | Forgejo（局域网） | `http://192.168.50.2:3000/gacloud/stm32-precision-timing-generator.git` | 内网 / 树莓派托管 |
| `gitea-pub` | Forgejo（公网隧道） | `https://frp-bus.com:32179/gacloud/stm32-precision-timing-generator.git` | 外网访问内网 Forgejo |

三者是**同一份代码的不同入口**，不是三个项目。默认分支均为 `main`，许可证 MIT。

---

## 二、克隆

任选其一（推荐 GitHub，最快）：

```sh
# GitHub（需要能访问外网）
git clone https://github.com/GacloudAir/stm32-precision-timing-generator.git

# 内网 Forgejo（在实验室网络内）
git clone http://192.168.50.2:3000/gacloud/stm32-precision-timing-generator.git

# 外网访问内网 Forgejo（自签证书，需跳过校验）
git -c http.sslVerify=false clone https://frp-bus.com:32179/gacloud/stm32-precision-timing-generator.git
```

若 clone 后只有 `origin`，按第一节用 `git remote add` 补上另外两个。

---

## 三、认证配置（一次性）

### GitHub
使用 **Personal Access Token (PAT)** 作为密码（账号填 GitHub 用户名）：
```sh
# Windows：让 Git Credential Manager 记住 PAT
git config --global credential.helper manager
# 首次 push/pull 时，用户名填 GacloudAir，密码粘贴 PAT
```
PAT 需具备 `repo` 权限。不要在命令行明文传递 token。

### Gitea（Forgejo）
- 内网 `gitea`：用户名 + 密码（或 token），凭据存 Windows 凭据管理器
- 公网 `gitea-pub`：需跳过自签证书校验（仅对该主机）：
  ```sh
  git config --global http.https://frp-bus.com:32179.sslVerify false
  git config --global credential.https://frp-bus.com:32179.provider generic
  ```
  并把内网已存的凭据复制过去，避免网页授权：
  ```sh
  printf "protocol=http\nhost=192.168.50.2:3000\n\n" | git credential fill \
    | sed 's#^protocol=.*#protocol=https#; s#^host=.*#host=frp-bus.com:32179#' \
    | git credential approve
  ```

**权限**：协作者需要写权限才能推送。账号 `Agent` 已被添加为写协作者（write）。

---

## 四、日常推送（核心）

**任何改动都要同时推送到 GitHub 和 Gitea，保持双端一致。** 推荐顺序：先内网（快、稳），再 GitHub（慢、可能需代理）。

```sh
# 1. 提交
git add . && git commit -m "说明"

# 2. 推内网 Gitea（最快）
git push gitea main

# 3. 推 GitHub（公网主源）
git push origin main

# 4.（可选）推公网 Gitea 隧道——若 gitea 已是最新，此步通常 Everything up-to-date
git push gitea-pub main
```

### 一键推双端（设为别名，可选）
```sh
git config --global alias.pushboth '!f() { git push gitea main && git push origin main; }; f'
# 之后只需： git pushboth
```

### 验证一致性
```sh
git ls-remote origin refs/heads/main    # GitHub
git ls-remote gitea refs/heads/main     # Gitea
# 两个哈希应相同，且等于本地 git rev-parse HEAD
```

---

## 五、网络与代理（重要）

GitHub 在国内直连常常超时或被阻断，需要走本地代理（Clash Verge）。

- Clash 的 mixed-port 默认是 `7897`（本机实测）；环境变量里可能设置的是别的端口（如 `7174`），以实际监听为准：
  ```sh
  netstat -ano -p tcp | grep LISTENING | grep 7897
  ```
- **Clash 核心进程可能会意外退出**，只剩 `clash-verge-service.exe`，此时代理端口无响应、git push 报 `502` 或 `schannel: server closed abruptly`。检查方法：
  ```sh
  tasklist | grep -i mihomo        # 应看到 verge-mihomo.exe
  ```
  若缺失，重新打开 Clash Verge 客户端即可恢复。
- 走代理推送（若环境变量已设 proxy 则用默认）：
  ```sh
  git -c http.proxy=http://127.0.0.1:7897 push origin main
  ```
- 网络不好时多试几次（GFW 间歇性封锁）：
  ```sh
  alias pushr='for i in 1 2 3 4; do git push "$@" && break; sleep 5; done'
  ```

---

## 六、固件源码说明（避免混淆）

| 文件 | 角色 |
| --- | --- |
| `firmware/target/main.c` | **Keil 工程实际编译的主文件**，现在是英文注释、含运行时调频功能 |
| `firmware/target/main_cn.c` | 中文注释参考版，代码逻辑与 `main.c` 完全一致，仅供阅读 |
| `firmware/target/uart_protocol_ref.c` | UART 协议参考笔记 |
| `firmware/target/timing-generator.uvprojx` | Keil MDK 工程，引用 `main.c` |

改动功能时**以 `main.c` 为准**，改完同步更新 `main_cn.c` 的注释；两者去注释后的代码应保持一致：
```sh
python3 - <<'PY'
import re
def code(p):
    s=open(p,encoding='utf-8',errors='replace').read()
    s=re.sub(r'/\*.*?\*/','',s,flags=re.S); s=re.sub(r'//.*','',s)
    return re.sub(r'\s+','',s)
print("一致" if code('firmware/target/main.c')==code('firmware/target/main_cn.c') else "不一致!")
PY
```

编译环境：Keil MDK / armclang V6.24，STM32F1xx DFP 2.4.1，预期 `0 Error, 0 Warning`。

---

## 七、边界提醒

- 本仓库是**独立开源项目**，与课题存档 `D:\Projects\STM32\well-logging` **完全无关**，不要混淆或混入任何课题内容（随钻超声 / MD1813 / 基金号等）。
- `well-logging` 为非开源、纯本地、零 remote，**严禁**推送到任何远端。
- 本仓库不含任何第三方安装包 / 版权资料 / 私钥；新增文件时遵守 `.gitignore`，不要提交 `*.hex`、`*.axf`、`Objects/`、`*.pem` 等。
