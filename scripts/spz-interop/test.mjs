import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {mkdirSync, readFileSync, writeFileSync} from 'node:fs';
import {join, resolve} from 'node:path';
import {fileURLToPath} from 'node:url';
import {decompress} from 'fzstd';

const [converterArg, coreTestsArg, outputArg] = process.argv.slice(2);
assert.ok(converterArg && coreTestsArg && outputArg,
    'usage: node test.mjs <splat-convert> <splat_core_tests> <output-directory>');
const converter = resolve(converterArg);
const coreTests = resolve(coreTestsArg);
const output = resolve(outputArg);
mkdirSync(output, {recursive: true});

// Exercise the pinned official parser's CPU path. Only expose its private parseSpz entry
// point and resolve its import; GPU resources, browser rendering and the WASM bridge are
// outside this host check. fzstd independently decompresses the actual attribute streams.
const playcanvasEntry = import.meta.resolve('playcanvas');
const parserPath = fileURLToPath(import.meta.resolve('playcanvas/scripts/esm/parsers/spz-parser.mjs'));
const parserSource = readFileSync(parserPath, 'utf8')
    .replace("from 'playcanvas';", `from '${playcanvasEntry}';`);
const {parseSpz} = await import(`data:text/javascript;base64,${Buffer.from(
    `${parserSource}\nexport {parseSpz};\n`).toString('base64')}`);
const decoder = {
    decompress(src, expectedSize) {
        const result = decompress(src);
        assert.equal(result.byteLength, expectedSize);
        return result;
    }
};
const parse = (path) => {
    const data = readFileSync(path);
    return parseSpz(data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength), decoder);
};
const run = (command, args, env = process.env) => {
    const result = spawnSync(command, args, {encoding: 'utf8', env});
    assert.ifError(result.error);
    assert.equal(result.status, 0, `${command}\n${result.stdout}\n${result.stderr}`);
};
const near = (actual, expected, tolerance) => assert.ok(Math.abs(actual - expected) <= tolerance,
    `${actual} differs from ${expected} by more than ${tolerance}`);

// Same asymmetric RDF pose as SplatConvertTest.cpp. PLY stores SH channel-major and its
// quaternion in wxyz order; the cloud and SPZ use coefficient-major SH and xyzw.
const position = [1.125, -2.375, 3.625];
const rotation = [0.2, 0.3, 0.4, Math.sqrt(0.71)];
const sh = Array.from({length: 45}, (_, i) => 0.1 + (i % 9) * 0.025);
const properties = ['x', 'y', 'z', 'f_dc_0', 'f_dc_1', 'f_dc_2',
    ...Array.from({length: 45}, (_, i) => `f_rest_${i}`), 'opacity',
    'scale_0', 'scale_1', 'scale_2', 'rot_0', 'rot_1', 'rot_2', 'rot_3'];
const values = [...position, 0.1, 0.2, 0.3];
for (let channel = 0; channel < 3; channel++) {
    for (let coefficient = 0; coefficient < 15; coefficient++) {
        values.push(sh[coefficient * 3 + channel]);
    }
}
values.push(Math.log(3), -1, -2, -3, rotation[3], ...rotation.slice(0, 3));
const header = Buffer.from(`ply\nformat binary_little_endian 1.0\nelement vertex 1\n${
    properties.map(name => `property float ${name}\n`).join('')}end_header\n`);
const body = Buffer.alloc(values.length * 4);
values.forEach((value, i) => body.writeFloatLE(value, i * 4));
const ply = join(output, 'oriented.ply');
writeFileSync(ply, Buffer.concat([header, body]));

const checkPose = (path, rub) => {
    const data = parse(path);
    assert.equal(data.numSplats, 1);
    assert.equal(data.shBands, 3);
    const centers = data.getCenters();
    [position[0], position[1] * (rub ? -1 : 1), position[2] * (rub ? -1 : 1)].forEach((expected, i) =>
        near(centers[i], expected, 1 / 4096));
    [-1, -2, -3].forEach((expected, i) => near(data.scales[i] / 16 - 10, expected, 1 / 16));
    near(data.alphas[0] / 255, 0.75, 1 / 255);
    [0.1, 0.2, 0.3].forEach((expected, i) => near((data.colors[i] / 255 - 0.5) / 0.15, expected, 0.02));
    // Decode the parser's raw smallest-three bytes and compare orientation up to q/-q.
    let packed = new DataView(data.rotations.buffer, data.rotations.byteOffset, 4).getUint32(0, true);
    const largest = packed >>> 30;
    const q = [0, 0, 0, 0];
    let squares = 0;
    for (let i = 3; i >= 0; i--) {
        if (i !== largest) {
            q[i] = (packed & 0x1ff) * (Math.SQRT1_2 / 511) * ((packed & 0x200) ? -1 : 1);
            packed >>>= 10;
            squares += q[i] * q[i];
        }
    }
    q[largest] = Math.sqrt(1 - squares);
    const expectedRotation = [rotation[0], rotation[1] * (rub ? -1 : 1), rotation[2] * (rub ? -1 : 1), rotation[3]];
    near(Math.abs(q.reduce((sum, value, i) => sum + value * expectedRotation[i], 0)), 1, 0.001);
    const signs = [-1, -1, 1, -1, 1, 1, -1, 1, -1, 1, -1, -1, 1, -1, 1];
    sh.forEach((expected, i) => near((data.sh[i] - 128) / 128, expected * (rub ? signs[Math.floor(i / 3)] : 1), 0.07));
};

const v4 = join(output, 'splatkit-rub-v4.spz');
run(converter, [ply, v4, '--spz-version', '4', '--target-frame', 'rub']);
checkPose(v4, true);
const v2 = join(output, 'splatkit-rdf-v2.spz');
run(converter, [ply, v2]);
assert.throws(() => parse(v2), /only version 4 is supported/);

const transformCli = fileURLToPath(new URL('node_modules/@playcanvas/splat-transform/bin/cli.mjs', import.meta.url));
const transformed = join(output, 'splat-transform-rdf-v4.spz');
run(process.execPath, [transformCli, '-w', ply, transformed]);
// SplatTransform 3.10.0 writes PLY/RDF coordinates with from: UNSPECIFIED. Assert the
// observed convention rather than inferring a frame from the v4 container or its vendor.
checkPose(transformed, false);
run(coreTests, ['--gtest_filter=SplatConvert.ImportsExternalOrReferenceSpz'],
    {...process.env, SPLAT_INTEROP_FIXTURE_PATH: transformed, SPLAT_INTEROP_SOURCE_FRAME: 'rdf'});
const transformedRub = join(output, 'splat-transform-normalized-rub-v4.spz');
run(converter, [transformed, transformedRub, '--spz-version', '4', '--target-frame', 'rub']);
checkPose(transformedRub, true);
run(coreTests, ['--gtest_filter=SplatConvert.ImportsExternalOrReferenceSpz'],
    {...process.env, SPLAT_INTEROP_FIXTURE_PATH: transformedRub, SPLAT_INTEROP_SOURCE_FRAME: 'rub'});

// Conversion from another supported external format uses the same real importer/exporter.
const splat = join(output, 'external.splat');
const splatBytes = Buffer.alloc(32);
position.forEach((value, i) => splatBytes.writeFloatLE(value, i * 4));
[-1, -2, -3].forEach((value, i) => splatBytes.writeFloatLE(Math.exp(value), 12 + i * 4));
[0.1, 0.2, 0.3].forEach((value, i) => { splatBytes[24 + i] = Math.round((0.5 + 0.282095 * value) * 255); });
splatBytes[27] = Math.round(0.75 * 255);
[rotation[3], ...rotation.slice(0, 3)].forEach((value, i) => { splatBytes[28 + i] = Math.round(value * 128 + 128); });
writeFileSync(splat, splatBytes);
const bridged = join(output, 'splat-bridge-rdf-v4.spz');
run(process.execPath, [transformCli, '-w', splat, bridged]);
const bridgeData = parse(bridged);
assert.equal(bridgeData.numSplats, 1);
assert.equal(bridgeData.shBands, 0);
position.forEach((expected, i) => near(bridgeData.getCenters()[i], expected, 1 / 4096));
console.log('SPZ interop passed: official PlayCanvas 2.23.1 CPU parser, SplatTransform 3.10.0, SDK decoder; no GPU/device validation.');
