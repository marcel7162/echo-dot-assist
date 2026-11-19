// hassmic's copy (README.md here): librespot's build script asked git (vergen-gitcl) for the commit and the clock for
// the date, and drew a random build id.  Inside this repository git would name hassmic's commit, and the speaker's
// binary would change with every commit (scripts/content-id.sh: a beta for every doc change).  Fixed values instead:
// what upstream's build gives with SOURCE_DATE_EPOCH=0 and VERGEN_IDEMPOTENT set.
fn main() {
    println!("cargo:rustc-env=VERGEN_GIT_SHA=v0.8.0");
    println!("cargo:rustc-env=VERGEN_GIT_COMMIT_DATE=VERGEN_IDEMPOTENT_OUTPUT");
    println!("cargo:rustc-env=VERGEN_BUILD_DATE=VERGEN_IDEMPOTENT_OUTPUT");
    println!("cargo:rustc-env=LIBRESPOT_BUILD_ID=0");
    println!("cargo:rerun-if-changed=build.rs");
}
