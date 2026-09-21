# Security

## Reporting a vulnerability

Report a vulnerability privately through GitHub's [private vulnerability reporting](https://github.com/Xget7/splatkit/security/advisories/new) or by email to juanieltupa@gmail.com.
Do not open a public issue for a security problem.
Include the affected artifact and version, the platform, and the smallest reproduction you can.

## Supported versions

Only the newest published alpha of each artifact is supported: the Maven Central `io.github.xget7:splatkit-android`, the npm `@splatkit/react-native`, and the `splatkit-ios` Swift package.
Pre-1.0 alphas are not patched in place; a fix ships as a new alpha.

## Binary distribution

The iOS package downloads a prebuilt `SplatKitCore.xcframework` and verifies it against the checksum pinned in `Package.swift`, and the React Native package verifies the same archive against the checksum in `packages/react-native-splatkit/scripts/ios-xcframework.json`.
A mismatch aborts the fetch.
Reports about those pins, the export filter that generates the public mirrors, or a leaked credential are in scope.
