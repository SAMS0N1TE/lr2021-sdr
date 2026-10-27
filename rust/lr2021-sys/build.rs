// SPDX-License-Identifier: GPL-3.0-only
// Compiles the repository's C sources. LR2021_SDR_SRC may point at another
// checkout; by default the crate builds from the workspace's repository root.
use std::path::PathBuf;

fn main() {
    let root = std::env::var_os("LR2021_SDR_SRC")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../.."));
    let src = root.join("src");
    let mut b = cc::Build::new();
    b.include(root.join("include")).flag_if_supported("-std=c11").warnings(false);
    for f in ["lr2021_iq.c", "lr2021_iq_dev.c", "lr2021_iq_tx.c", "lr2021_sim.c",
              "lr2021_bridge.c", "lr2021_bridge_server.c", "lriq.c", "lriq_fifo.c"] {
        b.file(src.join(f));
    }
    let unix = std::env::var("CARGO_CFG_TARGET_FAMILY").map(|f| f == "unix").unwrap_or(false);
    if unix {
        for f in ["host.c", "host_io.c", "host_linux_baud.c", "host_server.c", "host_spidev.c"] {
            b.file(src.join("host").join(f));
        }
        b.file("shim/host_shim.c");
        println!("cargo:rustc-link-lib=pthread");
        println!("cargo:rustc-link-lib=m");
    }
    b.file("shim/shim.c");
    b.compile("lr2021");
    println!("cargo:rerun-if-changed={}", src.display());
    println!("cargo:rerun-if-changed={}", root.join("include").display());
    println!("cargo:rerun-if-changed=shim");
    println!("cargo:rerun-if-env-changed=LR2021_SDR_SRC");
    println!("cargo:include={}", root.join("include").display());
}
