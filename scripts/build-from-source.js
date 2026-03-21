#!/usr/bin/env node
/* eslint no-console:0, no-process-env:0 */
"use strict";

const os = require("os");
const { spawnSync } = require("child_process");
const { platform, arch, buildFolder } = require("./build-vars.js");

const args = ["-O", buildFolder, "-a", arch];

if (process.env.DEBUG) {
  args.push(...["--debug", "--CDCMAKE_EXPORT_COMPILE_COMMANDS=1"]);
}

if (platform === "win32") {
  args.push(...["-G", "Ninja"]);
  // Force MSVC compiler. MinGW c++.exe on PATH is incompatible with
  // libwebrtc compiled by MSVC/clang-cl (dllexport, calling conventions).
  // vcvarsall.bat must be run before this script to put cl.exe on PATH.
  args.push("--CDCMAKE_C_COMPILER=cl", "--CDCMAKE_CXX_COMPILER=cl");
  // Explicitly set RC compiler to Windows rc.exe. Without this, CMake
  // finds node_modules/.bin/rc (npm config package) instead of the
  // Windows Resource Compiler.
  args.push("--CDCMAKE_RC_COMPILER=rc");
}

if (arch !== os.arch()) {
  args.push(
    `--CDCMAKE_TOOLCHAIN_FILE=toolchains/${platform}-${arch}.toolchain`
  );
}

function main() {
  console.log("Running cmake-js " + args.join(" "));
  let { status } = spawnSync("cmake-js", ["configure", ...args], {
    shell: true,
    stdio: "inherit",
  });
  if (status) {
    throw new Error("cmake-js configure failed for wrtc");
  }

  console.log("Running cmake-js build");
  status = spawnSync("cmake-js", ["build", ...args], {
    shell: true,
    stdio: "inherit",
  }).status;
  if (status) {
    throw new Error("cmake-js build failed for wrtc");
  }

  console.log("Built wrtc");
}

module.exports = main;

if (require.main === module) {
  main();
}
