"use strict";

const os = require("os");
const triple = `${os.platform()}-${os.arch()}`;
const pathsToTry = [
  `../build-${triple}/wrtc.node`,
  `../build-${triple}/Debug/wrtc.node`,
  `../build-${triple}/Release/wrtc.node`,
  `@omnicommsai/wrtc-${triple}`,
  // For installations that can't resolve node_modules directly, like AWS Lambda
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
