/* Prepare dark grayscale PNG inputs for the SDK's file-based EZIP builder. */
const fs = require('node:fs');
const path = require('node:path');
const sharp = require('sharp');
const root = path.resolve(__dirname, '..');
const sourceDir = path.join(root, 'assets/weather');
const pngDir = path.join(root, 'assets/ezip/weather');
const sizes = [160, 48];
const FOREGROUND_GRAY = 0x11;

async function main() {
    fs.mkdirSync(pngDir, {recursive: true});
    const files = fs.readdirSync(sourceDir).filter(name => /^w\d+\.png$/.test(name));
    if (!files.includes('w999.png') || files.length !== 70) throw new Error('Expected 70 weather icons, including w999');
    for (const file of files) {
        const code = Number(file.slice(1, -4));
        for (const size of sizes) {
            const {data, info} = await sharp(path.join(sourceDir, file))
                .resize(size, size, {fit: 'contain', background: {r: 255, g: 255, b: 255, alpha: 0}})
                .toColourspace('srgb').ensureAlpha().raw().toBuffer({resolveWithObject: true});
            if (info.channels !== 4) throw new Error(`Unexpected format: ${file}`);
            for (let i = 0; i < size * size; ++i) {
                data[i * 4] = data[i * 4 + 1] = data[i * 4 + 2] = FOREGROUND_GRAY;
                data[i * 4 + 3] = Math.round(data[i * 4 + 3] / 17) * 17;
            }
            await sharp(data, {raw: {width: size, height: size, channels: 4}})
                .png().toFile(path.join(pngDir, `w${code}_${size}.png`));
        }
    }
    console.log(`Prepared all ${files.length} weather icons at ${sizes.join('/')} px for EZIP`);
}
main().catch(error => {console.error(error); process.exitCode = 1;});
