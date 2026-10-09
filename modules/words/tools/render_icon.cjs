const path = require('node:path');
const fs = require('node:fs');
const sharp = require('sharp');

async function main() {
    const root = path.resolve(__dirname, '..');
    const output = path.join(root, 'output', 'assets');
    fs.mkdirSync(output, {recursive: true});
    const {data, info} = await sharp(path.join(root, 'assets', 'icon.svg'), {density: 384})
        .resize(80, 80).ensureAlpha().raw().toBuffer({resolveWithObject: true});
    for (let i = 0; i < info.width * info.height; ++i) {
        data[4 * i] = data[4 * i + 1] = data[4 * i + 2] = 0x22;
        data[4 * i + 3] = Math.round(data[4 * i + 3] / 17) * 17;
    }
    await sharp(data, {raw: info}).png().toFile(path.join(output, 'icon.png'));
}
main().catch(error => { console.error(error); process.exitCode = 1; });
