"use strict";

const os = require("os");
const triple = `${os.platform()}-${os.arch()}`;
const pathsToTry = [
  // Prebuilt binaries committed to repo (primary path for Docker/CI)
  `../prebuilds/${triple}/wrtc.node`,
  // Local build artifacts
  `../build-${triple}/wrtc.node`,
  `../build-${triple}/Debug/wrtc.node`,
  `../build-${triple}/Release/wrtc.node`,
  // Platform-specific npm packages
  `@omnicommsai/wrtc-${triple}`,
  `./node_modules/@omnicommsai/wrtc-${triple}`,
  `./node_modules/@omnicommsai/wrtc-${triple}/wrtc.node`,
];

let succeeded = false;
for (const path of pathsToTry) {
  try {
    module.exports = require(path);
    succeeded = true;
    break;
  } catch (error) {
    // Ignore any errors, just continue
    void error;
  }
}

if (!succeeded) {
  throw new Error(
    `Could not find wrtc binary on any of the paths: ${pathsToTry}`,
  );
}
