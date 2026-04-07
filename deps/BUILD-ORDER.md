# Prism — Dependency Build Order

Run these scripts in order from the VM (Z:\prism-browser\deps\).
Each script clones its own source and writes output to Z:\prism-browser\arm32\.

## Prerequisites (install once)

- Visual Studio with component: "MSVC vXXX - VS 20XX C++ ARM build tools"
- Go (amd64 host): https://go.dev/dl/  — required by BoringSSL code generation
- Git

## Step 1 — BoringSSL (TLS 1.3)

```powershell
.\build-boringssl-arm32.ps1
```

Produces: arm32\lib\ssl.lib, crypto.lib
          arm32\include\openssl\

## Step 2 — nghttp2 (HTTP/2)

```powershell
.\build-nghttp2-arm32.ps1
```

Produces: arm32\lib\nghttp2.lib
          arm32\include\nghttp2\

## Step 3 — libcurl (networking, linked to BoringSSL + nghttp2)

```powershell
.\build-curl-arm32.ps1
```

Produces: arm32\lib\libcurl.lib
          arm32\include\curl\

## Step 4 — WebKit dependencies (Cairo, FreeType, HarfBuzz, ICU, etc.)

Script: build-webkit-deps-arm32.ps1  [TODO — next phase]

## Step 5 — WebKit itself

CMake flags documented in ../CMakeLists.txt  [TODO — next phase]

## Link order (final app)

libcurl.lib ssl.lib crypto.lib nghttp2.lib ws2_32.lib crypt32.lib
