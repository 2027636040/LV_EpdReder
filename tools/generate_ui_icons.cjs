/* Prepare fixed-size transparent PNG inputs for the SDK EZIP resource builder. */
const fs = require('node:fs');
const path = require('node:path');
const sharp = require('sharp');
const root = path.resolve(__dirname, '..');
const icons = [
    ['bookshelf', '书架.svg', 80], ['weather', '天气.svg', 80],
    ['transfer', '文件传输.svg', 80], ['settings', '设置.svg', 80],
    ['album', '相册.svg', 80], ['wifi_entry', 'wifi状态-已连接.svg', 80],
    ['quick', '快捷信息.svg', 32], ['wifi_on', 'wifi状态-已连接.svg', 32],
    ['wifi_disconnected', 'wifi状态-未连接.svg', 32],
    ['bt_on', '蓝牙_bluetooth.svg', 32], ['bt_disconnected', '蓝牙状态-未连接.svg', 32],
    ['battery_empty', '电池没电_0-20.svg', 40],
    ['battery_mid', '电池运行_20-80.svg', 40],
    ['battery_full', '电池满电_80-100.svg', 40],
    ['battery_charge', '电池充电_battery-charge.svg', 40],
    ['key1', '按键一_one-key.svg', 28], ['key2', '按键二_two-key.svg', 28],
    ['key3', '按键三_three-key.svg', 28],
    ['back', '返回.svg', 32],
    ['location', '位置.svg', 32], ['humidity', '湿度.svg', 32],
    ['wind', '风速.svg', 32], ['visibility', '能见度.svg', 32],
    ['cloud', '云量.svg', 32], ['sunrise', '日出_sunrise.svg', 32],
    ['sunset', '日落.svg', 32], ['pressure', '气压.svg', 32],
    ['air', '空气质量.svg', 32],
];
async function main() {
    const out = path.join(root, 'assets/ezip/icons');
    fs.mkdirSync(out, {recursive: true});
    let header = '#ifndef UI_ICONS_H\n#define UI_ICONS_H\n#include "lvgl.h"\n';
    for (const [name, file, size] of icons) {
        const {data, info} = await sharp(path.join(root, 'assets/icons', file), {density: 384})
            .resize(size, size).ensureAlpha().raw().toBuffer({resolveWithObject: true});
        if (info.channels !== 4) throw new Error(`Unexpected channels: ${file}`);
        for (let i = 0; i < size * size; ++i) {
            data[i * 4] = data[i * 4 + 1] = data[i * 4 + 2] = 0x22;
            data[i * 4 + 3] = Math.round(data[i * 4 + 3] / 17) * 17;
        }
        await sharp(data, {raw: {width: size, height: size, channels: 4}})
            .png().toFile(path.join(out, `ui_icon_${name}.png`));
        header += `extern const lv_image_dsc_t ui_icon_${name};\n`;
    }
    fs.writeFileSync(path.join(root, 'src/ui/icons/ui_icons.h'), header + '#endif\n');
    console.log(`Prepared ${icons.length} transparent PNG icons for EZIP`);
}
main().catch(e => {console.error(e); process.exitCode = 1;});
