# Fuck My Shit Mountain Audit Report

**Project:** crosspoint-reader-cjk  
**Audit mode:** full  
**Date:** 2026-07-08  
**Reviewer:** Codex GPT-5

---

## 1. Executive Summary

本次审计覆盖 `crosspoint-reader-cjk` 的全仓库主要一方代码、PlatformIO 配置、GitHub Actions、文档、测试脚本、网络接口、持久化层和关键 reader/runtime 路径。项目的基础工程纪律不差：有格式化、cppcheck、默认固件构建 CI，有清晰的活动管理模型、SD 卡互斥封装、WebDAV PUT 临时文件写入、固件镜像基础校验和集中化的 reader refresh policy。但面向稳定公开发布，当前最大风险集中在网络边界和凭据边界。

最需要立即处理的是 Web 服务安全面：`/api/settings` 会把 KOReader 明文密码序列化给客户端，Web 文件/设置/Wi-Fi/OPDS/WebDAV 接口缺少鉴权，普通 HTTP 文件接口对 `.crosspoint` 等受保护路径的处理弱于 WebDAV；再叠加 OPDS/KOReader HTTPS 客户端关闭证书校验，真实同网段或设备 AP 场景下可以形成凭据泄漏、文件破坏、设置篡改和中间人攻击链。测试和发布链也没有覆盖这些高风险路径：CI 主要证明“能格式化、能 cppcheck、能 build”，不能证明网络边界、凭据红action、路径保护、OTA/OPDS/KOReader 行为正确。

结论：代码库不是不可维护的“烂摊子”，但安全边界存在发布阻断级问题。若要面向普通用户公开稳定发布，应先修复凭据泄漏、Web 鉴权和受保护路径访问，再补上 critical path 回归测试；随后处理 HTTPS 校验、原子持久化、release candidate workflow、供应链签名/校验和，以及拆分过大的 `CrossPointWebServer.cpp`。

### Score Dashboard

```
Security        ███░░░░░░░  3.0  C   Web 设置/文件接口缺少鉴权且会泄漏 KOReader 密码，HTTPS 客户端还关闭证书校验；安全覆盖为 High。
Stability       ██████░░░░  6.0  B   活动管理和存储互斥有基础设计，但 JSON 持久化非原子、dark-mode 直接刷新路径和 OTA 边界仍有风险；覆盖为 Medium-High。
Performance     ███████░░░  6.5  B   嵌入式内存压力有局部处理，但 Web 扫描/上传/下载缺少配额和大小上限，可能耗尽 SD/时间/内存；覆盖为 Medium。
Testing         ████░░░░░░  4.0  C   有 hyphenation/字体/渲染相关 host tests，但 CI 未覆盖 Web 安全、路径保护、TLS、持久化和 OTA 等关键路径；覆盖为 High。
Maintainability ██████░░░░  5.5  B   文档和模块划分有基础，但 `CrossPointWebServer.cpp` 1718 行混合多职责，边界规则已经出现分叉；覆盖为 High。
Design          █████░░░░░  5.2  B   reader refresh policy、ActivityManager 等设计较清楚，但 API 边界 fail-fast、SRP、显式依赖和最小权限原则被网络层破坏；覆盖为 Medium-High。
Release         █████░░░░░  5.0  B   CI 能构建默认/发布固件，但 RC workflow 引用不存在的 env，产物无签名/校验和/SBOM，且本地无法复跑验证；覆盖为 High。
────────────────────────────────────
Overall         █████░░░░░  5.0  B
```

Each dimension scored 0.0-10.0. **Higher = better (10 = clean, 0 = shit mountain).** Scores are judgment-based, not formula-based. See `rubrics/scoring.md` for anchor descriptions.

### Finding Statistics

| Severity | Count | Confirmed | Suspected |
|----------|-------|-----------|-----------|
| Critical | 1 | 1 | 0 |
| High | 4 | 4 | 0 |
| Medium | 6 | 6 | 0 |
| Low | 0 | 0 | 0 |
| Info | 0 | 0 | 0 |
| **Total** | **11** | **11** | **0** |

## 2. Project Map

`crosspoint-reader-cjk` 是面向 Xteink X4 / ESP32-C3 的 PlatformIO + Arduino 固件。入口在 `src/main.cpp`，启动后初始化硬件、设置、状态、字体、活动管理和网络能力。主要业务层包括 reader activities、home/settings/network activities、EPUB/TXT/XTC 解析和渲染、OPDS 浏览、KOReader 同步、Web 文件服务、WebDAV、Wi-Fi 凭据、OPDS/KOReader 凭据、OTA/SD 恢复更新和 I18n 生成资产。

主要边界如下：

- Runtime entry points: `src/main.cpp`、activity 生命周期、WebServer/WebSocket 回调、WebDAV raw/HTTP handlers、OTA/SD recovery workflow。
- Architecture boundaries: `src/activities/*` 负责交互流程，`lib/Epub/*` 负责解析/排版/hyphenation，`lib/GfxRenderer/*` 负责 framebuffer/text/image 渲染，`src/network/*` 负责 Web/HTTP/OTA，`src/JsonSettingsIO.cpp` 和 store classes 负责 SD JSON 持久化。
- Data flow: 输入按钮/蓝牙翻页 -> `ActivityManager` -> activity render/action；Web 请求 -> `CrossPointWebServer`/`WebDAVHandler` -> SD/设置 store；OPDS/KOReader -> HTTP client -> parser/store；OTA -> GitHub release metadata/firmware -> firmware installer。
- State ownership: `SETTINGS`、`APP_STATE`、`I18N`、`KOREADER_STORE`、`WIFI_CREDENTIAL_STORE`、`OPDS_SERVER_STORE`、`RECENT_BOOKS` 等 singleton-style globals 是既有约定；多数持久化状态落在 `.crosspoint/*.json`。
- Persistence layer: `JsonSettingsIO` 读写 settings/state/wifi/koreader/opds/recent JSON，旧 `.bin` 会迁移到 `.bak`；WebDAV PUT 使用 `.davtmp`，但常规 JSON save 直接 `Storage.writeFile`。
- Privacy-sensitive data: Wi-Fi SSID/密码、OPDS server credentials、KOReader username/password/server URL、阅读进度/最近阅读、文件名和目录结构。
- External interfaces: HTTP port 80, WebSocket 81, WebDAV, UDP discovery, OPDS HTTP(S), KOReader sync HTTP(S), GitHub OTA, SD file system, GitHub Actions release artifacts。
- Security boundaries: 目前主要依赖“同网段/设备 AP 可访问”而非应用层鉴权；WebDAV 有 protected path helper，普通 HTTP 文件 API 没有同等保护。
- Testing structure: `test/` 有 hyphenation eval、rounding、external font host-side 脚本；CI 运行格式、cppcheck 和 build，不运行 Web/security/persistence/update tests。
- Release process: GitHub Actions 构建 default/gh_release/gh_release_tc/recovery；release 上传固件二进制；RC workflow 引用未定义 PlatformIO env。
- Cost/resource drivers: ESP32-C3 RAM、e-paper refresh、SD I/O、HTTP/WebSocket uploads、OPDS/OTA downloads、large EPUB/image/font parsing。

Coverage note: 本次使用 `rg --files` 建立文件清单，重点审计一方源码、CI、PlatformIO、文档、测试脚本和高风险输入/持久化/网络路径。排除或仅抽样检查生成文件、字体/图片二进制、minified `jszip.min.js`、hyphenation generated assets、`open-x4-sdk/` submodule 和 vendored `lib/expat`/`lib/uzlib`。当前本地环境缺少可用 `python`/`python3`/`py`、`pio`、`clang-format`、`bash`，因此未能运行构建、cppcheck、hyphenation eval 或 skill report lint；结论以源码和配置证据为主。

### Coverage Matrix

| Dimension | Coverage | Evidence inspected | Exclusions / limits |
|-----------|----------|--------------------|---------------------|
| Architecture | High | `src/main.cpp`, `src/activities/**`, `src/network/**`, `lib/Epub/**`, `lib/GfxRenderer/**`, docs architecture | 未做运行时依赖图生成；生成/第三方代码排除 |
| Security | High | Web routes, settings API, WebDAV path checks, OPDS/KOReader HTTP clients, OTA configs, docs endpoints | 未跑动态渗透/设备 AP 实测 |
| Stability | Medium | ActivityManager, HalStorage, JsonSettingsIO, OTA, reader refresh paths | 未做断电/SD 故障/设备长稳测试 |
| Performance | Medium | Web file listing/upload, parser/render hot areas by source inspection | 未做设备 profiling、large EPUB benchmark |
| Testing | High | `.github/workflows/*`, `test/**`, docs contributing, AGENTS commands | 未运行测试，因为本机缺少 Python/bash/toolchain |
| Maintainability | High | largest first-party files, network/settings/persistence modules, docs and generator boundaries | 未量化 cyclomatic complexity |
| Design | Medium | SRP/fail-fast/boundary contracts/global state/error handling patterns | 未对全部函数逐个套 rubric |
| Release | High | `platformio.ini`, CI/release/RC/recovery workflows, artifact steps | 未实际执行 release workflow |
| Documentation | Medium | README, USER_GUIDE, docs webserver/contributing/recovery/architecture | 未做链接检查和截图校验 |
| Observability | Medium | logging macros usage, Web/OTA/storage error logs, docs troubleshooting | 嵌入式无 metrics/tracing runtime；未接设备日志 |
| Configuration | Medium | settings list, JSON load/save, PlatformIO envs, generated config flow | 未 fuzz 所有配置组合 |
| Data-Integrity | High | JsonSettingsIO saves, storage wrappers, migration, WebDAV temp write | 未做断电注入测试 |
| Privacy | High | Wi-Fi/OPDS/KOReader credentials, settings API, file listing/download | 未检查真实用户 SD 数据 |
| Accessibility | Low | Web HTML/source-level UX paths, device UI activities by source only | 未跑浏览器、screen reader、键盘焦点或设备可用性测试 |
| Supply-Chain | Medium | GitHub Actions, PlatformIO deps, release artifacts, submodule status | 未跑 osv/trivy/syft/cosign |
| Cost | Medium | local resource caps for SD/RAM/network operations | 无云成本；未做压力测试 |
| AI-Safety | Not assessed | 搜索未发现 LLM/prompt/RAG/tool-call surface | 项目无 AI/LLM 功能面 |
| Fallback | Medium | settings migration/load defaults, OTA response fallback, reader low-memory fallback | 未完整覆盖每个 fallback branch |
| Testing-Authenticity | High | CI/test scripts vs critical production flows | 未运行 tests，但测试覆盖缺口可从配置确认 |
| Type-Safety | Medium | C++ boundary types, JSON parsing, `String`/`std::string`, numeric parsing | 未用 clang-tidy/ubsan/asan |
| Frontend-State | Low | `src/network/html/*.html`, route/API coupling, minified JS excluded | 未做浏览器交互验证；非 SPA framework |
| Backend-API | High | `CrossPointWebServer.cpp`, `WebDAVHandler.cpp`, `docs/webserver-endpoints.md` | 未做 HTTP integration tests |
| Dependency-Weight | Medium | `platformio.ini`, vendored libs, generated/minified assets | 未计算 binary size deltas |
| Code-Consistency | Medium | naming/includes/error/logging patterns in core modules | 未跑 clang-format/cppcheck locally |
| Comment-Coverage | Medium | docs and inline comments in network/storage/OTA/reader policy | 未全面审查每个 public API doc |

## 3. Top Risks

| Priority | Finding | Severity | Summary |
|----------|---------|----------|---------|
| 1 | `/api/settings` 暴露 KOReader 明文密码 | Critical | 设置列表把 `koPassword` 作为普通 DynamicString 返回，GET 设置接口会序列化给任何 Web 客户端。 |
| 2 | Web 服务缺少鉴权，暴露文件/设置/凭据修改面 | High | `/upload`、`/delete`、`/api/settings`、Wi-Fi/OPDS API、WebDAV 等均在同一 WebServer 上无 auth 注册。 |
| 3 | 普通 HTTP 文件接口可触达受保护 `.crosspoint` 路径 | High | WebDAV 检查每个路径段，普通 listing/download/upload/delete 只做局部 basename/隐藏项过滤。 |
| 4 | OPDS/KOReader HTTPS 关闭证书校验 | High | `setInsecure()` 保护不了 Basic Auth / KOReader headers / 下载内容免受同网段 MITM。 |
| 5 | 关键网络/安全/更新路径没有 CI 回归测试 | High | CI 只覆盖格式、cppcheck、build；没有 settings redaction、path protection、auth、TLS、atomic save、OTA tests。 |
| 6 | 设置/状态/凭据 JSON 非原子写入 | Medium | `JsonSettingsIO::*save*` 直接 `Storage.writeFile`，断电/SD 失败可能截断配置或凭据文件。 |
| 7 | Release candidate workflow 引用不存在的 PlatformIO env | Medium | `release_candidate.yml` 构建 `gh_release_rc*`，但 `platformio.ini` 未定义这些环境。 |
| 8 | Web 文件操作缺少大小/配额/速率边界 | Medium | HTTP/WebSocket upload 和目录扫描可被同网段客户端用来耗尽 SD/时间/内存。 |
| 9 | `CrossPointWebServer.cpp` 过大且混合多职责 | Medium | 1718 行文件同时处理路由、文件、设置、OPDS、Wi-Fi、WebSocket、UDP 和安全 helper。 |
| 10 | 发布供应链缺少产物 provenance、签名和 checksum | Medium | Actions 多处 tag/floating version，release 只上传二进制，无 checksum/SBOM/signature。 |
| 11 | dark-mode refresh policy 仍有直接 `HALF_REFRESH` 绕过 | Medium | 中央 reader policy 已处理 dark mode，但多个可见 UI/error/fallback 路径直接调用 half refresh。 |

## 4. Detailed Findings

### Finding: `/api/settings` exposes the KOReader password in plaintext

- Severity: Critical
- Confidence: High
- Category: Security
- Status: Confirmed
- Affected area: Web settings API / KOReader credential store
- Evidence:
  - File: `src/SettingsList.h:93-106`
  - Function / Module: `getSettingsList()`
  - Relevant behavior: `koPassword` is declared as `SettingInfo::DynamicString` and its getter returns `KOREADER_STORE.getPassword()`.
  - File: `src/network/CrossPointWebServer.cpp:1226-1323`
  - Function / Module: `CrossPointWebServer::handleGetSettings`
  - Relevant behavior: each string setting with `s.stringGetter` is serialized into `doc["value"]`.
- Problem: KOReader password is modeled like a normal editable string setting, so `GET /api/settings` can return the clear password to any HTTP client that reaches the embedded WebServer.
- Why it matters: This is direct credential disclosure. The same credential is later used for KOReader sync and Basic Auth-compatible sync servers, so a Web UI read-only request can become account compromise.
- Realistic failure scenario: A user enables the device Web server on home Wi-Fi or AP mode. Another client on that network opens `/api/settings`, reads `koPassword`, then uses the KOReader sync credentials against the configured server or reuses the password elsewhere.
- Minimal fix: Mark password settings as secret and omit `value` from `handleGetSettings`; return `hasValue`/`isConfigured` only. Accept new password values only on POST and never echo them.
- Better long-term fix: Introduce a typed settings schema with `sensitivity`, redaction policy, write-only fields, API serialization tests, and a single credential API that never exposes secret material.
- Regression test suggestion: Add a host or HTTP integration test that seeds KOReader credentials, calls the settings serialization path, and asserts the response contains `koPassword` metadata but no clear password or obfuscated secret.
- Estimated effort: 1-3 hours for redaction and test; 1-2 days for typed secret-field schema.

### Finding: Web server exposes mutation endpoints without application-level authentication

- Severity: High
- Confidence: High
- Category: Security
- Status: Confirmed
- Affected area: HTTP/WebDAV/WebSocket API surface
- Evidence:
  - File: `src/network/CrossPointWebServer.cpp:248-287`
  - Function / Module: `CrossPointWebServer::begin`
  - Relevant behavior: routes register `/upload`, `/mkdir`, `/rename`, `/move`, `/delete`, `/api/settings`, `/api/wifi/*`, `/api/opds/*`, WebDAV and upload WebSocket handlers without auth middleware or token checks.
  - File: `docs/webserver-endpoints.md:133-212`
  - Function / Module: Web server API documentation
  - Relevant behavior: documents direct `curl` access to upload, mkdir and delete endpoints over plain HTTP.
- Problem: The embedded WebServer exposes file mutation, settings mutation, Wi-Fi credential mutation, OPDS credential mutation, and WebDAV operations based only on network reachability.
- Why it matters: Same-network access is not an authorization boundary. AP mode also creates a local network where any connected client can modify or delete SD contents and change credentials/settings.
- Realistic failure scenario: The user starts Web server for file transfer. A second device on the same Wi-Fi or AP posts to `/delete` for book files, posts `/api/settings` to change reader behavior, or saves attacker-controlled OPDS credentials/server URLs.
- Minimal fix: Add a device-generated admin token/session for all mutating endpoints and secret-bearing reads; require explicit pairing or on-device confirmation before enabling Web management.
- Better long-term fix: Split public status/download surfaces from admin surfaces, add CSRF/origin protections where browser forms are used, and default WebDAV/mutation APIs to disabled until explicitly enabled for a bounded time window.
- Regression test suggestion: Add HTTP route tests that assert unauthenticated POSTs to upload/delete/settings/Wi-Fi/OPDS/WebDAV mutation endpoints return 401/403 and authenticated requests still work.
- Estimated effort: 1-2 days for a simple token gate; 1-2 weeks for pairing/session/CSRF/time-bound enablement.

### Finding: Plain HTTP file endpoints do not enforce the same protected-path boundary as WebDAV

- Severity: High
- Confidence: High
- Category: Security
- Status: Confirmed
- Affected area: File browser, download, upload, delete and storage path handling
- Evidence:
  - File: `src/network/WebDAVHandler.cpp:761-785`
  - Function / Module: `WebDAVHandler::isProtectedPath`
  - Relevant behavior: WebDAV checks every path segment and blocks dot-prefixed/hidden protected directories.
  - File: `src/network/CrossPointWebServer.cpp:561-609`
  - Function / Module: `CrossPointWebServer::handleFileListData`
  - Relevant behavior: accepts query `path`, prepends `/` if needed, then scans that directory without rejecting a protected parent segment.
  - File: `src/network/CrossPointWebServer.cpp:611-667`
  - Function / Module: `CrossPointWebServer::handleDownload`
  - Relevant behavior: rejects hidden final names, but a path like `/.crosspoint/settings.json` has final basename `settings.json`.
  - File: `lib/FsHelpers/FsHelpers.cpp:9-43`
  - Function / Module: `FsHelpers::normalisePath`
  - Relevant behavior: normalizes slashes and `..`, but does not enforce protected segments.
- Problem: A security rule exists in WebDAV but is not shared by the normal HTTP file API. The file browser/download/upload/delete paths can address sensitive `.crosspoint` storage if callers supply the parent path directly.
- Why it matters: `.crosspoint` stores settings, state, recent books, Wi-Fi credentials, OPDS credentials and KOReader credentials. Listing, downloading, overwriting or deleting these files can disclose private data or break the device.
- Realistic failure scenario: An HTTP client requests `/api/files?path=/.crosspoint`, then downloads `/.crosspoint/koreader.json` or deletes `/.crosspoint/settings.json`, causing credential exposure or reset/corruption on next boot.
- Minimal fix: Move `isProtectedPath` into a shared path policy used by WebDAV and all normal HTTP handlers before any `Storage.open`, `scanFiles`, upload, rename, move or delete.
- Better long-term fix: Introduce a `StoragePathPolicy` with canonicalization, protected-segment checks, operation-specific permissions, tests, and a deny-by-default API for all web-exposed file operations.
- Regression test suggestion: Add tests for `/api/files`, `/download`, upload, move, rename and delete using `/.crosspoint`, `/.hidden/file`, nested hidden segments and encoded path variants; assert all are rejected.
- Estimated effort: 4-8 hours for shared helper and tests; 2-4 days for full path policy refactor.

### Finding: OPDS and KOReader HTTPS clients disable certificate verification

- Severity: High
- Confidence: High
- Category: Security
- Status: Confirmed
- Affected area: OPDS downloads, KOReader sync, credential transport
- Evidence:
  - File: `src/network/HttpDownloader.cpp:54-75`
  - Function / Module: `HttpDownloader::fetchUrl`
  - Relevant behavior: creates `NetworkClientSecure`, calls `setInsecure()`, then may send Basic Auth credentials.
  - File: `src/network/HttpDownloader.cpp:103-126`
  - Function / Module: `HttpDownloader::downloadToFile`
  - Relevant behavior: downloads book content over HTTPS with `setInsecure()` and optional Basic Auth.
  - File: `lib/KOReaderSync/KOReaderSyncClient.cpp:18-25`
  - Function / Module: `addAuthHeaders`
  - Relevant behavior: sends `x-auth-user`, `x-auth-key` and HTTP Basic Auth using stored credentials.
  - File: `lib/KOReaderSync/KOReaderSyncClient.cpp:41-51`, `lib/KOReaderSync/KOReaderSyncClient.cpp:79-89`, `lib/KOReaderSync/KOReaderSyncClient.cpp:142-151`
  - Function / Module: KOReader sync requests
  - Relevant behavior: for HTTPS URLs, constructs `WiFiClientSecure` and calls `setInsecure()` before sending auth headers.
- Problem: HTTPS is used without validating server certificates, so encryption does not authenticate the server.
- Why it matters: Same-network attackers can impersonate OPDS or KOReader servers, capture credentials, tamper OPDS feeds, replace downloaded books, and manipulate sync progress.
- Realistic failure scenario: On hotel/home Wi-Fi, an attacker performs DNS spoofing or captive-portal-style interception. The device accepts the forged TLS endpoint, sends KOReader credentials and downloads attacker-controlled content.
- Minimal fix: Use ESP certificate bundle or pinned server CA/fingerprint for HTTPS; at minimum make insecure TLS an explicit per-server opt-in with warning and never send credentials over insecure mode by default.
- Better long-term fix: Store per-server TLS policy, support CA import/pinning, validate hostname, and include connection-security state in OPDS/KOReader settings UI.
- Regression test suggestion: Unit-test URL/client selection to assert HTTPS paths configure a verifying secure client; add an integration test with a self-signed/mock certificate path that fails unless explicitly trusted.
- Estimated effort: 1-3 days depending on certificate bundle/pinning UX; longer if custom CA import is required.

### Finding: Critical network, credential, persistence and update paths are not covered by CI tests

- Severity: High
- Confidence: High
- Category: Testing
- Status: Confirmed
- Affected area: CI and regression test suite
- Evidence:
  - File: `.github/workflows/ci.yml:12-115`
  - Function / Module: CI workflow
  - Relevant behavior: runs clang-format, cppcheck, charset generation check and `pio run`; no HTTP/API/security/persistence/update test suite is executed.
  - File: `AGENTS.md:43-56`
  - Function / Module: repository test guidance
  - Relevant behavior: explicitly states there is no regular `pio test` suite enforced in CI and the active test path is hyphenation evaluation.
  - File: `test/run_hyphenation_eval.sh:5-15`
  - Function / Module: host-side hyphenation eval
  - Relevant behavior: compiles hyphenation-specific code, not Web/server/persistence/security paths.
- Problem: The riskiest production surfaces have no automated regression tests in CI: settings redaction, authentication, path authorization, WebDAV/HTTP policy parity, TLS verification, atomic settings save, and OTA release handling.
- Why it matters: The current CI can stay green while credential leaks, path traversal/protected-path bypasses, or insecure TLS regressions are introduced or remain unfixed.
- Realistic failure scenario: A future settings UI change adds another password-like field to `SettingsList.h`; CI builds successfully, but `/api/settings` starts leaking a new credential because no redaction test exists.
- Minimal fix: Add focused host tests or small integration harnesses for settings serialization, protected path policy, route auth gating, TLS policy selection and JSON atomic write behavior.
- Better long-term fix: Create a CI test tier for critical boundaries: host unit tests for pure policies, fake storage/WebServer tests for APIs, and nightly hardware/smoke tests for OTA/Web flows.
- Regression test suggestion: Start with tests named `SettingsApiSecretsAreRedacted`, `ProtectedPathPolicyRejectsHiddenSegments`, `MutatingRoutesRequireAuth`, `HttpsClientsVerifyCertificates`.
- Estimated effort: 1-2 days for first focused tests; 1-2 weeks for a robust harness.

### Finding: Settings, state and credential JSON files are written directly instead of atomically

- Severity: Medium
- Confidence: High
- Category: Stability
- Status: Confirmed
- Affected area: Persistent settings/state/credentials on SD card
- Evidence:
  - File: `src/JsonSettingsIO.cpp:70-82`
  - Function / Module: `JsonSettingsIO::saveState`
  - Relevant behavior: serializes JSON and directly returns `Storage.writeFile(path, json)`.
  - File: `src/JsonSettingsIO.cpp:118-148`, `src/JsonSettingsIO.cpp:251-260`, `src/JsonSettingsIO.cpp:289-302`, `src/JsonSettingsIO.cpp:337-350`, `src/JsonSettingsIO.cpp:381-395`
  - Function / Module: settings, KOReader, Wi-Fi, recent books and OPDS save functions
  - Relevant behavior: all current JSON saves directly overwrite target files.
  - File: `src/network/WebDAVHandler.cpp:80-106`
  - Function / Module: WebDAV PUT
  - Relevant behavior: WebDAV uses a `.davtmp` temp file and rename pattern, proving an atomic-ish local pattern exists.
- Problem: Power loss, reset, SD write failure or partial writes can leave `.crosspoint/*.json` truncated or malformed.
- Why it matters: These files are the source of truth for credentials, settings, state and recent books. Corruption can lose Wi-Fi/OPDS/KOReader credentials, break boot-time defaults, or cause confusing fallback behavior.
- Realistic failure scenario: User changes Wi-Fi or KOReader settings, then battery dies during `Storage.writeFile`. On next boot the JSON parser fails and the store falls back to default/migration behavior, losing credentials or state.
- Minimal fix: Add a shared `writeJsonAtomic(path, json)` that writes `path.tmp`, flushes/closes, optionally keeps `path.bak`, then renames to the target only after success.
- Better long-term fix: Add versioned persisted records with checksum, backup restore, migration rollback, and boot-time reconciliation/reporting of corrupted files.
- Regression test suggestion: Fake `Storage.writeFile` partial failure and assert the old JSON remains readable; test tmp/backup recovery after simulated interrupted rename.
- Estimated effort: 1 day for shared atomic write; 2-4 days for backup/restore tests and migration hardening.

### Finding: Release candidate workflow builds PlatformIO environments that do not exist

- Severity: Medium
- Confidence: High
- Category: Release
- Status: Confirmed
- Affected area: GitHub Actions release candidate workflow
- Evidence:
  - File: `.github/workflows/release_candidate.yml:39-59`
  - Function / Module: release candidate build
  - Relevant behavior: runs `pio run -e gh_release_rc` and `pio run -e gh_release_rc_tc`, then copies artifacts from `.pio/build/gh_release_rc*`.
  - File: `platformio.ini:78-106`
  - Function / Module: PlatformIO env definitions
  - Relevant behavior: defines `default`, `gh_release`, `gh_release_tc` and `recovery`; no `gh_release_rc` or `gh_release_rc_tc`.
- Problem: The RC workflow is structurally broken and will fail before producing artifacts.
- Why it matters: A release candidate path that cannot run weakens release readiness and encourages bypassing RC validation or manually producing untracked artifacts.
- Realistic failure scenario: A maintainer triggers RC on a `release/*` branch; GitHub Actions fails at the first missing environment, so no candidate firmware is generated for validation.
- Minimal fix: Add `[env:gh_release_rc]` and `[env:gh_release_rc_tc]` that extend the intended release configs, or update the workflow to build existing `gh_release`/`gh_release_tc` envs.
- Better long-term fix: Use a matrix over declared release envs, validate env names in CI, and make RC/release workflows share one build artifact path.
- Regression test suggestion: Add a lightweight CI script that parses `platformio.ini` env names and verifies every workflow `pio run -e <env>` references a defined env.
- Estimated effort: 30 minutes-2 hours for env/workflow fix; 1 day for reusable release matrix validation.

### Finding: Web file operations lack size, quota and rate boundaries

- Severity: Medium
- Confidence: High
- Category: Performance
- Status: Confirmed
- Affected area: HTTP upload, WebSocket upload, file listing and SD storage
- Evidence:
  - File: `src/network/CrossPointWebServer.cpp:561-609`
  - Function / Module: `handleFileListData`
  - Relevant behavior: scans caller-provided directories and streams entries with no traversal budget or pagination.
  - File: `src/network/CrossPointWebServer.cpp:717-874`
  - Function / Module: HTTP upload handling
  - Relevant behavior: accepts upload chunks to SD without an application-level max file size or quota gate.
  - File: `src/network/CrossPointWebServer.cpp:1643-1828`
  - Function / Module: WebSocket upload handling
  - Relevant behavior: parses client-provided size and manages upload session without a hard configured max payload or rate limit.
- Problem: Any client that can reach the Web server can force long directory scans, large writes, repeated uploads, or SD fill-up.
- Why it matters: On ESP32-C3 and SD storage, unbounded work can block the UI loop, trigger watchdog pressure, exhaust storage, or make the file server unavailable.
- Realistic failure scenario: A browser tab or script repeatedly uploads large files over HTTP/WebSocket until SD is full; subsequent settings saves, book downloads or state writes fail.
- Minimal fix: Enforce max upload size, free-space checks, per-session timeout, request count/rate budget and directory listing pagination/depth limits.
- Better long-term fix: Add a Web transfer manager with admission control, progress cancellation, storage reserve for `.crosspoint`, and backpressure surfaced to the UI.
- Regression test suggestion: Simulate upload sizes above the configured cap, low free-space conditions and huge directory listing requests; assert rejection before writes start.
- Estimated effort: 1-2 days for caps/free-space checks; 3-5 days for transfer manager and UI states.

### Finding: `CrossPointWebServer.cpp` is a multi-responsibility boundary module

- Severity: Medium
- Confidence: High
- Category: Maintainability
- Status: Confirmed
- Affected area: Embedded web/API layer
- Evidence:
  - File: `src/network/CrossPointWebServer.cpp:248-287`
  - Function / Module: route registration
  - Relevant behavior: registers status, file, upload, settings, Wi-Fi, OPDS, WebDAV and WebSocket routes in one module.
  - File: `src/network/CrossPointWebServer.cpp:561-1219`
  - Function / Module: file listing/download/upload/move/rename/delete
  - Relevant behavior: implements file management and storage mutation.
  - File: `src/network/CrossPointWebServer.cpp:1226-1525`
  - Function / Module: settings API
  - Relevant behavior: serializes and mutates typed settings.
  - File: `src/network/CrossPointWebServer.cpp:1527-1995`
  - Function / Module: OPDS/Wi-Fi/WebSocket/credential APIs
  - Relevant behavior: mixes independent API domains and upload state machines.
- Problem: One 1718-line source file owns routing, file security policy, settings serialization, credential APIs, WebSocket transfer, OPDS management, Wi-Fi credential management, UDP discovery and WebDAV delegation.
- Why it matters: The protected-path bug is a concrete symptom: WebDAV and regular HTTP handlers evolved separate policy implementations. Large boundary modules make it easy to fix one surface and miss another.
- Realistic failure scenario: A developer adds a new storage mutation endpoint and copies the nearest handler pattern, but misses the auth/protected-path logic that lives elsewhere in the same large file.
- Minimal fix: Extract shared policies first: `WebAuthPolicy`, `StoragePathPolicy`, `SettingsApiSerializer`, and use them from the existing file without changing route behavior.
- Better long-term fix: Split into `FileApi`, `SettingsApi`, `CredentialApi`, `OpdsApi`, `WifiApi`, `UploadSession` and `DiscoveryService`, with route registration as composition.
- Regression test suggestion: Add policy-level tests before refactor; after extraction, run route contract tests to ensure all endpoints still return the same successful responses and reject protected paths consistently.
- Estimated effort: 2-4 days for policy extraction; 1-2 weeks for module split with tests.

### Finding: Release supply chain lacks artifact integrity and provenance controls

- Severity: Medium
- Confidence: High
- Category: Release
- Status: Confirmed
- Affected area: CI/CD, dependencies and release artifacts
- Evidence:
  - File: `.github/workflows/ci.yml:48-50`, `.github/workflows/release.yml:23-25`, `.github/workflows/release_candidate.yml:20-22`, `.github/workflows/sd_recovery.yml:25-27`
  - Function / Module: GitHub Actions tool setup
  - Relevant behavior: `astral-sh/setup-uv@v7` is used with `version: "latest"`.
  - File: `.github/workflows/release.yml:68-84`
  - Function / Module: release artifact upload
  - Relevant behavior: uploads `firmware-sc.bin` and `firmware-tc.bin` to artifacts/release without checksum, signature, SBOM or provenance attestation.
  - File: `platformio.ini:28-46`
  - Function / Module: dependency configuration
  - Relevant behavior: some dependencies are pinned, while at least `bitbank2/PNGdec @ ^1.0.0` permits semver drift.
- Problem: Release artifacts are not independently verifiable by users, and parts of CI/tool/dependency resolution can shift over time.
- Why it matters: Firmware distribution benefits from strong provenance. Without checksums/signatures/SBOM and tighter pins, users and maintainers have less ability to detect tampered artifacts or reproduce a release.
- Realistic failure scenario: A dependency or setup tool changes behavior under the same workflow tag/range; release artifacts are produced and uploaded, but there is no signed checksum/provenance to compare or validate later.
- Minimal fix: Generate SHA256 checksum files for every firmware/recovery artifact, publish them in releases, and pin `setup-uv` version plus dependency versions more tightly where feasible.
- Better long-term fix: Add SBOM generation, GitHub artifact attestations or cosign signatures, reproducible build documentation, and a release verification guide.
- Regression test suggestion: Add workflow checks that fail if release artifacts are uploaded without matching `.sha256` files and if dependency/action pins violate repository policy.
- Estimated effort: 2-4 hours for checksums/version pinning; 2-5 days for SBOM/signing/provenance.

### Finding: Dark-mode refresh policy is bypassed by direct half-refresh calls

- Severity: Medium
- Confidence: High
- Category: Stability
- Status: Confirmed
- Affected area: Reader/UI refresh paths
- Evidence:
  - File: `lib/ReaderRuntime/ReaderRuntimePolicy.cpp:47-96`
  - Function / Module: reader refresh policy
  - Relevant behavior: central policy routes dark-mode reader updates to `RefreshMode::DarkRedrive`.
  - File: `src/main.cpp:403-409`
  - Function / Module: manual force refresh
  - Relevant behavior: calls `renderer.displayBuffer(HalDisplay::HALF_REFRESH)` directly.
  - File: `src/activities/reader/EpubReaderActivity.cpp:625-640`, `src/activities/reader/EpubReaderActivity.cpp:734-740`
  - Function / Module: EPUB error/low-memory fallback rendering
  - Relevant behavior: visible fallback paths call `HALF_REFRESH`.
  - File: `src/activities/boot_sleep/SleepActivity.cpp:138-207`, `src/activities/boot_sleep/SleepActivity.cpp:306-308`
  - Function / Module: sleep screens
  - Relevant behavior: visible sleep render paths call `HALF_REFRESH`.
- Problem: Repository policy says dark mode should not introduce visible `HALF_REFRESH`/ordinary `FAST_REFRESH` paths unless documented and device-tested. Several direct calls bypass the central policy and do not branch on dark mode.
- Why it matters: On e-paper dark mode, half refresh can whiten/fade the dark background or worsen ghosting. Because these are fallback/sleep/error paths, they may be missed in normal manual testing.
- Realistic failure scenario: User reads in dark mode, hits an EPUB memory fallback or sleep transition, and the display partially refreshes with `HALF_REFRESH`, leaving a degraded dark background until a later full/redrive refresh.
- Minimal fix: Replace visible direct half-refresh calls with a helper that checks `renderer.isDarkMode()` and uses `displayBufferDarkRedrive()` or an explicit `RefreshMode::DarkRedrive` equivalent.
- Better long-term fix: Remove direct display-mode calls from activities except through a small refresh policy facade, and document any hardware-tested exception in code.
- Regression test suggestion: Add a compile-time or grep-based CI check for direct `HALF_REFRESH`/`FAST_REFRESH` calls in reader/UI paths, plus unit tests for dark-mode refresh decision helpers.
- Estimated effort: 3-6 hours for helper and call-site fixes; 1-2 days for CI guard and device verification.

## 5. Architecture Concerns

- Coverage: High
- Inspected evidence: `src/main.cpp`, `src/activities/**`, `src/network/**`, `lib/Epub/**`, `lib/GfxRenderer/**`, `docs/contributing/architecture.md`.
- Exclusions / limits: no runtime dependency graph or device-level trace was generated; submodule/vendor/generated assets were excluded.

The architecture is understandable and has useful boundaries between activities, renderer, parser/layout libraries, storage and network. The weak boundary is the Web/API layer: `CrossPointWebServer.cpp` acts as router, controller, serializer, credential API, file policy and transfer manager. This is where security policy drift already happened between WebDAV and normal HTTP handlers.

| Concern | Evidence | Impact |
|---------|----------|--------|
| BoundaryContract | F-03 | Protected path rules are not a shared contract across WebDAV and HTTP file APIs. |
| StateOwnership | Project globals | Existing singleton globals match repo convention, but make test seams harder for settings/network. |
| EvolutionRisk | F-09 | Adding Web features has high blast radius because unrelated API domains share one file. |

## 6. Security Concerns

- Coverage: High
- Inspected evidence: Web routes, settings API, WebDAV protected paths, HTTP clients, OTA config, docs endpoint examples.
- Exclusions / limits: no live network testing, fuzzing, credential scanning tools, or device AP test.

Security is the release-blocking dimension. F-01 through F-04 form a practical chain: unauthenticated Web client can read settings, settings include plaintext KOReader password, file endpoints can touch `.crosspoint`, and HTTPS clients do not authenticate servers. The smallest secure path is not a rewrite: redact secret settings, gate mutating/secret APIs with a token, centralize protected path checks, and stop using insecure TLS for credential-bearing requests.

## 7. Stability Concerns

- Coverage: Medium
- Inspected evidence: `ActivityManager`, `HalStorage`, `JsonSettingsIO`, OTA response handlers, reader refresh fallbacks.
- Exclusions / limits: no brownout/SD-failure/long-running hardware tests.

The code has stability-oriented patterns: render locks/pending actions avoid obvious activity races, SD operations are wrapped with a FreeRTOS mutex, WebDAV writes use temp/rename, and firmware installer validates basic image properties. Remaining systemic risk is around partial persistence writes and edge render/update paths that are hard to hit manually.

## 8. Performance Concerns

- Coverage: Medium
- Inspected evidence: Web listing/upload/download flows, parser/render module sizes, storage operations.
- Exclusions / limits: no heap profiling, benchmark EPUB corpus, or watchdog timing capture.

Performance risk is less about algorithmic hot spots and more about missing budgets at external boundaries. The Web server can be asked to scan arbitrary directories and accept large/repeated uploads without quota checks; on ESP32-C3 plus SD card this is a real resource-exhaustion and UX-stall risk.

## 9. Testing Gaps

- Coverage: High
- Inspected evidence: `.github/workflows/ci.yml`, `test/**`, AGENTS test guidance, docs contributing.
- Exclusions / limits: local tests could not be run because the current machine lacks Python/bash/toolchain commands.

The existing tests are valuable where they exist, especially hyphenation and font/render-adjacent host scripts. They do not protect the highest-risk release blockers. CI should gain a small set of host tests around pure security/persistence policies before broad hardware testing is attempted.

## 10. Maintainability Concerns

- Coverage: High
- Inspected evidence: largest first-party files, Web/settings/network modules, docs/generator boundaries.
- Exclusions / limits: no automated complexity metric output.

Generated/protected areas are documented well, and most class-oriented files use clear names. The main maintainability debt is concentrated: Web/API code and a few large rendering/parser files. `CrossPointWebServer.cpp` should be split only after shared policy tests exist, otherwise refactor risk will be high.

## 11. Design / Principles Concerns

- Coverage: Medium
- Inspected evidence: SRP, fail-fast, resource-bound, global dependency and boundary-contract examples.
- Exclusions / limits: not every function was scored against every principle.

The strongest principle violations are not stylistic. They cause observed bugs: SRP/file-size pressure in WebServer, fail-fast missing at API/path boundaries, and least-privilege violations in unauthenticated mutation routes. Reader refresh policy is a positive design, but direct call sites need to be brought under it.

## 12. Type Safety Concerns

- Coverage: Medium
- Inspected evidence: JSON setting parsing, `String`/`std::string` boundaries, WebSocket size parsing, settings type switch.
- Exclusions / limits: no clang-tidy/ubsan/asan, no fuzzing.

C++/Arduino boundary types require extra discipline. The current code uses typed `SettingInfo` variants, which is good, but security-sensitive metadata such as “secret/write-only” is missing from the type model. WebSocket upload size parsing from strings and JSON conversion should also be bounded and validated explicitly.

## 13. Release Concerns

- Coverage: High
- Inspected evidence: `platformio.ini`, CI/release/RC/recovery workflows, artifact upload steps.
- Exclusions / limits: release workflows were not executed.

Default release workflows are present and useful, but RC is broken by missing envs. Artifact integrity is also underdeveloped for firmware: publish checksums at minimum, then move toward signatures/SBOM/provenance.

## 14. Documentation Analysis

- Coverage: Medium
- Inspected evidence: README, USER_GUIDE, docs webserver/endpoints/recovery/contributing/architecture.
- Exclusions / limits: no link checker, no screenshot freshness check.

Docs are better than average for an embedded project: build commands, user guide, web endpoints, recovery flow and contributor workflow exist. The risky documentation gap is security expectation: docs show unauthenticated HTTP examples but do not make the access boundary explicit, and test docs still reference PlatformIO unit-test intent while AGENTS says CI does not enforce regular `pio test`.

## 15. Observability / Operability Analysis

- Coverage: Medium
- Inspected evidence: `LOG_DBG/LOG_INF/LOG_ERR` usage in network/storage/OTA paths and troubleshooting docs.
- Exclusions / limits: no device logs or production incident data.

Logging exists and is used consistently enough for embedded debugging. There are no metrics/tracing/health checks, which is normal for this class of firmware, but Web transfer/auth failures and persisted settings corruption should log enough context without leaking credentials.

## 16. Configuration Safety Analysis

- Coverage: Medium
- Inspected evidence: `SettingsList.h`, `JsonSettingsIO.cpp`, settings stores, PlatformIO envs.
- Exclusions / limits: no exhaustive config matrix tests.

Settings load code clamps many values and has compatibility migration. The missing safety property is secret classification. Config values that are credentials should not travel through the same read serialization path as ordinary UI preferences.

## 17. Data Integrity Analysis

- Coverage: High
- Inspected evidence: JSON save/load, binary migrations, WebDAV temp write pattern, storage wrappers.
- Exclusions / limits: no physical SD fault injection.

The data integrity issue is specific and fixable: current JSON writes overwrite target files directly. Adopt the temp/rename style already used in WebDAV and reserve SD space for `.crosspoint` writes so large uploads cannot starve settings persistence.

## 18. Privacy / Data Governance Analysis

- Coverage: High
- Inspected evidence: Wi-Fi/OPDS/KOReader credentials, settings API, file listing/download behavior.
- Exclusions / limits: no inspection of real user SD-card data.

Privacy risk is dominated by credential exposure through Web APIs and protected-path bypasses. Even if obfuscation is used on disk for some credentials, the Web settings API and file download surface can still disclose or damage sensitive user data.

## 19. Accessibility / UX Correctness Analysis

- Coverage: Low
- Inspected evidence: Web HTML sources and activity names/flows by source inspection.
- Exclusions / limits: no browser, axe, keyboard, screen reader, responsive or on-device UX verification.

No high-confidence accessibility finding is reported because runtime verification was not performed. UX correctness risks overlap with stability: dark-mode half-refresh fallback paths can visibly degrade the screen, and Web transfer errors should expose clear user states when quotas/auth are added.

## 20. Supply Chain / Reproducibility Analysis

- Coverage: Medium
- Inspected evidence: workflows, `platformio.ini`, submodule status, release artifact steps.
- Exclusions / limits: no vulnerability scanner, SBOM generator, or reproducible rebuild comparison.

The project has some pinned dependencies and a fixed PlatformIO core URL, which helps. Remaining gaps are artifact verification, floating tool setup version, semver-ranged dependency resolution and absence of SBOM/signing/provenance.

## 21. Cost / Resource Economics Analysis

- Coverage: Medium
- Inspected evidence: Web upload/listing, SD persistence, local resource flows.
- Exclusions / limits: no cloud cost because firmware is local; no stress tests.

Cost here means device resources. Web operations need storage reserve, max upload size, rate limiting, session timeout and listing pagination so a convenience file-transfer feature cannot consume the resources needed for the reader itself.

## 22. AI / LLM Safety Analysis

- Coverage: Not assessed
- Inspected evidence: project file inventory and dependency/config scan did not show prompts, model calls, RAG/vector stores, tool-call policies or LLM evals.
- Exclusions / limits: dimension not applicable to current codebase.

No AI/LLM safety findings are applicable.

## 23. Fallback / Defensive Code Analysis

- Coverage: Medium
- Inspected evidence: JSON load defaults/migrations, OTA response handling, reader low-memory/error paths.
- Exclusions / limits: fallback branches were not exhaustively executed.

Fallbacks should be more visible where user data is involved. Settings parse failure, credential-store corruption, insecure TLS fallback and dark-mode refresh fallback should either fail clearly or alert the user rather than silently continuing with unsafe/default behavior.

## 24. Testing Authenticity Analysis

- Coverage: High
- Inspected evidence: tests, CI, repository test guidance.
- Exclusions / limits: tests not executed locally due missing toolchain.

CI green currently means “format/static/build passed,” not “critical behaviors are safe.” Existing host tests are not fake, but they cover a narrow feature slice. The report therefore treats testing as poor for release confidence even though the available tests are useful.

## 25. Frontend State Analysis

- Coverage: Low
- Inspected evidence: `src/network/html/*.html`, generated HTML build pipeline references, API coupling.
- Exclusions / limits: minified JS excluded, browser state not exercised, no frontend framework state tree.

The Web UI is not a modern SPA with complex state management, so no framework-specific state finding is reported. The main frontend/backend coupling concern is that UI pages depend on unsafe backend endpoints; when auth/redaction/quota are added, Web UI request states must be updated accordingly.

## 26. Backend API Analysis

- Coverage: High
- Inspected evidence: `CrossPointWebServer.cpp`, `WebDAVHandler.cpp`, `docs/webserver-endpoints.md`.
- Exclusions / limits: no HTTP integration runtime.

The backend API lacks a clear distinction between public reads, secret reads and mutations. Response/error shapes are mostly ad hoc text/JSON. Before adding new endpoints, centralize auth, path policy, validation, size limits and error responses.

## 27. Dependency Weight Analysis

- Coverage: Medium
- Inspected evidence: `platformio.ini`, vendored libs, generated/minified assets and large library directories.
- Exclusions / limits: no binary-size delta analysis.

Dependency choices mostly match embedded needs: ArduinoJson, WebSockets, JPEG/PNG, expat/uzlib-like parsing and generated fonts/hyphenation data. Weight risk should be managed through binary size CI and release artifact size tracking rather than removals without evidence.

## 28. Code Consistency Analysis

- Coverage: Medium
- Inspected evidence: C++ naming, logging macros, include/layout conventions in core files, AGENTS style guide.
- Exclusions / limits: clang-format/cppcheck unavailable locally.

The code generally follows project conventions. The consistency issue with real impact is policy duplication: WebDAV and HTTP file APIs implement related storage access rules differently. Shared helpers should be preferred over copy-adjacent handler logic.

## 29. Comment Coverage Analysis

- Coverage: Medium
- Inspected evidence: comments around WebDAV protected paths, OTA buffers, reader refresh policy, docs.
- Exclusions / limits: not every public class/function was checked for docs.

Comments are useful where they explain hardware or compatibility constraints. Missing comments matter most for exceptions: if a dark-mode visible path must use `HALF_REFRESH`, the hardware reason and device-test result should be documented at the call site.

---

## 30. Principles Compliance

The codebase follows some important principles well: hardware/runtime constraints are documented, generated files are identified, and central policy exists for reader refresh decisions. The violated principles below are reported because they create concrete security, reliability or release risk.

### Principles Violated

| Principle | Violations | Severity | Affected Areas |
|-----------|------------|----------|----------------|
| Single Responsibility (1.1) | 1 major | Medium | `src/network/CrossPointWebServer.cpp` |
| File Size Limit (1.2) | 1 major | Medium | `src/network/CrossPointWebServer.cpp` at 1718 lines |
| Fail-Fast (4.4) | 3 | High | settings API secret serialization, protected path handling, Web upload bounds |
| Principle of Least Privilege (4.6) | 2 | High | unauthenticated mutation endpoints, secret-bearing settings reads |
| No Hidden Side Effects (5.3) | 1 | Medium | settings GET returns credential values through generic string getter |
| Explicit Dependencies Over Globals (7.3) | systemic but accepted local pattern | Medium | settings/network tests depend on singleton stores |
| Timeout/Bound Every External Resource (10.2/10.4) | 2 | Medium | Web uploads/listing, HTTPS clients |
| Configuration Over Hardcoding / Release Config Validity (9.1) | 1 | Medium | RC workflow env names drift from `platformio.ini` |

### Principles Respected

- Activity lifecycle and render locking are explicit enough to reduce action/render races.
- `HalStorage` centralizes SD mutex access instead of scattering raw SD calls everywhere.
- WebDAV PUT uses a temp-file/rename pattern that can be reused for settings persistence.
- Firmware installer performs basic image magic/chip/partition validation before OTA write.
- Generated/protected paths are documented in AGENTS and generator scripts are wired into PlatformIO.

---

## 31. Architecture Analysis

### Architecture Summary

| Subtype | Count | Affected Areas | Recommended Action |
|---------|-------|----------------|-------------------|
| ModuleBoundary | 1 | Web/API layer | Extract shared policies and route-domain modules |
| DependencyDirection | 0 | None confirmed | Keep hardware/framework coupling at boundary modules |
| StateOwnership | 1 | singleton stores | Add policy-level pure functions for testability |
| BoundaryContract | 2 | settings serialization, storage paths | Add typed secret/path contracts |
| EvolutionRisk | 1 | `CrossPointWebServer.cpp` | Split after tests are in place |

Primary architecture findings: F-03 and F-09.

## 32. Documentation Analysis

### Documentation Summary

| Subtype | Count | Affected Docs | Recommended Action |
|---------|-------|---------------|-------------------|
| UserDocs | 1 | webserver docs | Document auth/pairing once implemented |
| OperatorDocs | 1 | release/recovery docs | Add artifact verification steps |
| DeveloperDocs | 1 | testing docs | Align `test/README` and CI reality |
| ApiDocs | 1 | `docs/webserver-endpoints.md` | Document auth, error schema, secret redaction |
| DecisionRecord | 1 | missing ADR | Record Web security boundary decision |
| StaleDocs | 1 | `test/README` | Update PlatformIO test-runner language |

## 33. Privacy / Data Governance Analysis

### Privacy Summary

| Subtype | Count | Affected Data | Recommended Action |
|---------|-------|---------------|-------------------|
| DataInventory | 1 | `.crosspoint/*.json` | Document credential/state files and sensitivity |
| Minimization | 0 | None confirmed | Avoid adding new persisted secrets |
| AccessBoundary | 3 | KOReader, Wi-Fi, OPDS, files | Redact, authenticate, protect paths |
| Retention | 1 | recent books/state | Document retention/delete semantics |
| Deletion | 1 | file/settings API | Prevent unauthorized deletion |
| Export | 1 | file download | Block protected exports |
| TelemetryPrivacy | 0 | no external telemetry found | Keep logs redacted |

## 34. Accessibility / UX Correctness Analysis

### Accessibility Summary

| Subtype | Count | Affected Workflows | Recommended Action |
|---------|-------|-------------------|-------------------|
| SemanticStructure | 0 | Not verified | Run browser/source accessibility pass |
| KeyboardFocus | 0 | Not verified | Add manual/browser check if Web UI grows |
| ResponsiveVisual | 0 | Not verified | Check mobile Web transfer pages |
| ErrorState | 1 | Web upload/auth/quota future states | Add clear client errors |
| LoadingState | 1 | Web uploads | Prevent duplicate/oversized transfer actions |
| UXStateCorrectness | 1 | dark mode fallback | Route visible refreshes through dark policy |

## 35. Supply Chain / Reproducibility Analysis

### Supply Chain Summary

| Subtype | Count | Affected Surface | Recommended Action |
|---------|-------|------------------|-------------------|
| DependencyProvenance | 1 | PlatformIO deps | Tighten version pins where practical |
| Reproducibility | 1 | tool setup/version drift | Pin `setup-uv` version and document rebuilds |
| CIIntegrity | 1 | workflows | Add env-name validation and least-privilege permissions review |
| ArtifactProvenance | 1 | release firmware bins | Add checksums, signing/attestations, SBOM |
| RegistryHygiene | 0 | no package publishing found | Not applicable |

## 36. Cost / Resource Economics Analysis

### Cost Summary

| Subtype | Count | Cost Driver | Recommended Action |
|---------|-------|-------------|-------------------|
| UnboundedWork | 2 | Web uploads and directory scans | Add caps, pagination, timeout, quota |
| ExternalApiCost | 0 | OPDS/KOReader/GitHub are user-triggered | Add timeouts/retry budgets if background polling grows |
| LLMCost | 0 | no LLM usage | Not applicable |
| InfrastructureSizing | 1 | ESP32-C3 RAM/SD reserve | Reserve storage for settings/state |
| ObservabilityCost | 0 | local logs only | Keep logs concise |
| CostVisibility | 1 | transfer/storage usage | Show free space and rejected size in UI/API |

## 37. AI / LLM Safety Analysis

### AI Safety Summary

| Subtype | Count | Boundary Crossed | Recommended Action |
|---------|-------|------------------|-------------------|
| PromptInjection | 0 | None | Not applicable |
| ToolAuthorization | 0 | None | Not applicable |
| RAGLeakage | 0 | None | Not applicable |
| ModelFallback | 0 | None | Not applicable |
| OutputValidation | 0 | None | Not applicable |
| EvalGap | 0 | None | Not applicable |
| AbuseCost | 0 | None | Not applicable |

## 38. Observability / Operability Analysis

### Signal Summary

| Subtype | Count | Critical Signals Missing | Recommended Action |
|---------|-------|--------------------------|-------------------|
| Logging | 2 | auth failures, protected-path rejects | Log safe reason/path class without secrets |
| Metrics | 0 | embedded local firmware | Not required unless telemetry is added |
| Tracing | 0 | embedded local firmware | Not required |
| HealthCheck | 1 | Web server readiness/storage free space | Add status fields for storage/auth state |
| Alerting | 0 | no backend service | Not applicable |
| Runbook | 1 | corrupted settings/recovery | Document recovery after JSON corruption |
| Debuggability | 1 | Web upload failures | Return structured error codes |

## 39. Configuration Safety Analysis

### Configuration Summary

| Subtype | Count | Affected Keys / Files | Recommended Action |
|---------|-------|-----------------------|-------------------|
| SchemaValidation | 1 | settings JSON | Keep clamps and add tests for rejected values |
| UnsafeDefault | 1 | Web management enabled surfaces | Require explicit authenticated enablement |
| EnvironmentSeparation | 1 | RC envs | Align workflow and `platformio.ini` |
| SecretConfig | 1 | `koPassword`, Wi-Fi/OPDS | Add secret metadata/redaction |
| FeatureFlag | 0 | none confirmed | Add owner/expiry only for future flags |
| ConfigDocs | 1 | Web/API settings | Document secret handling and auth |

## 40. Data Integrity Analysis

### Integrity Summary

| Subtype | Count | Invariants at Risk | Recommended Action |
|---------|-------|-------------------|-------------------|
| TransactionBoundary | 1 | JSON target file must remain valid | Add temp/rename atomic writes |
| Idempotency | 1 | repeated upload/delete requests | Add safe retry/error behavior |
| ConcurrencyConsistency | 1 | Web and UI settings writes | Keep storage mutex and test concurrent saves |
| MigrationSafety | 1 | old `.bin` to `.json` | Preserve backup and test restartable migration |
| InvariantValidation | 1 | typed settings ranges/secrets | Add schema tests |
| BackupRestore | 1 | `.crosspoint` recovery | Add `.bak` restore on parse failure |
| Reconciliation | 1 | recent/state/files | Add boot-time corruption reporting |

## 41. Fallback / Defensive Code Analysis

### Fallback Summary

| Subtype | Count | KeepWithAlert | FailFast | Remove |
|---------|-------|---------------|----------|--------|
| SilentFallback | 2 | 1 | 1 | 0 |
| EmptyCatch | 0 | 0 | 0 | 0 |
| CompatibilityBranch | 2 | 2 | 0 | 0 |
| SilentCorrection | 1 | 1 | 0 | 0 |
| DefensiveGuess | 1 | 0 | 1 | 0 |

Settings migration/defaults are reasonable compatibility branches, but credential/path/TLS fallbacks should fail clearly rather than silently relaxing security.

## 42. Testing Authenticity Analysis

### Confidence Assessment

| Test Area | Real Confidence | Risk | Action |
|-----------|---------------|------|--------|
| Hyphenation eval | High for hyphenation | Does not cover Web/security | Keep and optionally wire to CI |
| Differential rounding | Medium for text metrics | Narrow host coverage | Keep |
| External font compatibility | Medium | Narrow compatibility coverage | Keep |
| CI build/cppcheck | Medium | Proves compilation/static baseline, not behavior | Keep and add behavior tests |
| Web/security/persistence/OTA | None | Critical bugs escape | Add focused tests |

### Valuable Tests

Hyphenation evaluation and host-side rendering/font tests are useful because they exercise actual code paths with representative fixtures.

### Suspicious Tests

No over-mocked tests were identified. The issue is absence, not fake green tests.

### Missing Tests

Missing tests include settings secret redaction, route auth, protected path rejection, HTTPS verification policy, atomic persistence, upload quotas and release workflow env validation.

---

## 43. Type Safety Analysis

### Summary

| Subtype | Count | Critical | High | Medium | Low |
|---------|-------|----------|------|--------|-----|
| UnsafeBlock | 0 | 0 | 0 | 0 | 0 |
| TypeAssertion | 0 | 0 | 0 | 0 | 0 |
| InputBoundary | 3 | 0 | 2 | 1 | 0 |
| OutputLeak | 1 | 1 | 0 | 0 | 0 |
| BooleanTrap | 0 | 0 | 0 | 0 | 0 |
| StringlyTyped | 2 | 0 | 0 | 2 | 0 |
| ErrorType | 1 | 0 | 0 | 1 | 0 |

The largest type-safety gap is not memory unsafe code; it is missing domain types for secrets, protected paths and bounded transfer sizes.

## 44. Frontend State Analysis

### Summary

| Subtype | Count | Affected Components |
|---------|-------|-------------------|
| ComponentSize | 0 | none verified |
| StateDuplication | 0 | none verified |
| PropDrilling | 0 | not applicable |
| EffectChain | 0 | not applicable |
| UIBusinessCoupling | 1 | Web UI to unsafe backend APIs |
| DOMasState | 0 | not verified |
| RequestState | 1 | upload/auth/quota states |
| RenderPerf | 0 | not verified |

Frontend findings are low-confidence due no browser verification. Backend API fixes will require matching Web UI state updates.

## 45. Backend API Analysis

### Summary

| Subtype | Count | Affected Endpoints |
|---------|-------|-------------------|
| ApiConsistency | 1 | mixed text/JSON errors |
| Validation | 2 | file paths, upload sizes |
| Auth | 1 | all mutating/secret endpoints |
| NplusOne | 0 | not applicable |
| Caching | 0 | not applicable |
| ErrorResponse | 1 | upload/delete/settings |
| BusinessLogic | 1 | settings serialization owns secret policy implicitly |
| DataFlow | 1 | `.crosspoint` files reachable via HTTP file API |

## 46. Dependency Weight Analysis

### Dependency Scoreboard

| Dependency | Status | Weight | Transitives | Used For | Recommended Action |
|------------|--------|--------|-------------|----------|-------------------|
| ArduinoJson @ 7.4.2 | Healthy | embedded library | n/a | settings/API JSON | Keep |
| Links2004/WebSockets @ 2.6.1 | Healthy but security-sensitive | embedded library | n/a | WebSocket upload | Keep, gate with auth/quota |
| bitbank2/PNGdec @ ^1.0.0 | Version range drift | embedded image decoder | n/a | PNG decode | Pin exact version if reproducibility is required |
| JPEGDEC pinned git SHA | Healthy | embedded image decoder | n/a | JPEG decode | Keep |
| vendored expat/uzlib | Needs ownership clarity | source vendored | n/a | XML/zip/inflate | Track upstream/security notes |
| generated fonts/hyphenation | Heavy but expected | large assets | n/a | CJK/font/hyphenation | Keep generated/protected policy |

---

## 47. Recommended Fix Order

### Fix Immediately

| Order | Item | Why |
|-------|------|-----|
| 1 | Redact `koPassword` and any future secret settings from `/api/settings` | Direct credential leak |
| 2 | Add authentication/token gate to mutating and secret-bearing Web/WebDAV/WebSocket endpoints | Same-network clients can mutate files/settings/credentials |
| 3 | Share protected path policy across WebDAV and normal HTTP file endpoints | `.crosspoint` files are sensitive state |
| 4 | Stop using `setInsecure()` for credential-bearing HTTPS by default | Prevent MITM credential/content compromise |

### Fix Before Stable Release

| Item | Why |
|------|-----|
| Add critical path CI tests for settings redaction, auth, path policy, TLS policy and atomic writes | Prevent regressions |
| Implement atomic JSON persistence with backup/restore | Prevent SD/power-loss corruption |
| Fix release candidate workflow env mismatch | Restore RC validation path |
| Add upload/listing size, quota and timeout controls | Avoid resource exhaustion |
| Publish SHA256 checksums for firmware artifacts | Let users verify releases |

### Schedule Later

| Item | Why |
|------|-----|
| Split `CrossPointWebServer.cpp` by route domain after policy tests exist | Reduce change risk |
| Add SBOM/signing/provenance and dependency pinning policy | Improve supply-chain posture |
| Add dark-mode refresh CI guard and device verification checklist | Keep display policy from regressing |
| Add browser/accessibility verification for Web UI | Useful after auth/quota states change |

### Ignore for Now

| Item | Why |
|------|-----|
| Full rewrite or dependency purge | Current risks are localizable; rewrite would add risk without first fixing boundaries |
| AI safety controls | No AI/LLM surface exists in this repo |

## 48. Quick Wins

| Quick win | Expected value | Effort |
|-----------|----------------|--------|
| Return no `value` for `koPassword`; return `configured: true/false` | Removes the most severe credential leak | 1-3 hours |
| Move WebDAV `isProtectedPath` semantics into shared helper and call it in HTTP handlers | Blocks `.crosspoint` exposure/deletion | 4-8 hours |
| Add a simple admin token check around all POST/mutation routes | Reduces unauthorized mutation risk quickly | 1 day |
| Add workflow check that every `pio run -e` env exists in `platformio.ini` | Prevents RC workflow drift | 1-2 hours |
| Generate `.sha256` files beside release firmware artifacts | Improves release verification immediately | 1-2 hours |
| Add grep/CI guard for direct visible `HALF_REFRESH` in dark-mode-sensitive paths | Prevents display policy regression | 1-2 hours |

## 49. Long-term Refactor Plan

1. Stabilize policies first.
   - Motivation: current bugs are boundary-policy failures.
   - Approach: introduce pure/testable `SecretSettingsPolicy`, `StoragePathPolicy`, `WebAuthPolicy` and `TransferLimitPolicy`.
   - Risk: low if route behavior is otherwise unchanged.
   - Testing strategy: host tests for each policy and route-level rejection tests.

2. Split the Web API layer after tests are green.
   - Motivation: lower feature-change blast radius and avoid duplicated security logic.
   - Approach: extract `FileApi`, `SettingsApi`, `CredentialApi`, `OpdsApi`, `WifiApi`, `UploadSession`, with one route-composition module.
   - Risk: medium because many endpoints share storage/server state.
   - Testing strategy: contract tests for existing endpoints plus protected-path/auth tests.

3. Harden persistence and release supply chain.
   - Motivation: prevent data loss and make firmware artifacts verifiable.
   - Approach: atomic JSON writes with backups, release checksums, env validation, later SBOM/signing.
   - Risk: low-medium; storage migration needs careful rollback.
   - Testing strategy: fake storage failure tests, workflow validation, checksum artifact checks.

