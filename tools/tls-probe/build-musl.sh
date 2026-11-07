# Builds the probe for aarch64-unknown-linux-musl with std from source,
# dynamic libc, global-dynamic TLS, and a shared-object final link.
export TMPDIR=/data/storage/el2/base/cache
export PATH=$HOME/.harmonybrew/bin:$PATH
export RUSTC_BOOTSTRAP=1
export CC=gcc CARGO_TARGET_AARCH64_UNKNOWN_LINUX_MUSL_LINKER=gcc CARGO_TARGET_AARCH64_UNKNOWN_LINUX_OHOS_LINKER=gcc
export CARGO_TARGET_AARCH64_UNKNOWN_LINUX_MUSL_RUSTFLAGS="-C target-feature=-crt-static -Z tls-model=global-dynamic -L native=$HOME/.harmonybrew/opt/ohos-sdk-native/llvm/lib/aarch64-linux-ohos"
export LIBRARY_PATH=$HOME/.harmonybrew/opt/ohos-sdk-native/llvm/lib/aarch64-linux-ohos
cd "$(dirname "$0")"
cargo rustc --release --target aarch64-unknown-linux-musl -Z build-std=std,panic_abort -- -C link-arg=-shared
