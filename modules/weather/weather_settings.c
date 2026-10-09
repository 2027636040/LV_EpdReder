#include "weather_ui.h"

static char city_draft[WEATHER_CITY_ID_MAX];
static bool city_draft_valid;
void settings_city_committed(void)
{
    city_draft_valid = false;
    current->city_committed = true;
}

void city_input_pause(void)
{
    if (current->city_committed) return;

    snprintf(city_draft, sizeof(city_draft), "%s", current->city_digits);
    city_draft_valid = true;
}
bool city_action(int target)
{
    if (launcher_current_page() == UI_PAGE_CITY_INPUT)
    {
        size_t length = strlen(current->city_digits);
        if (target >= CITY_DIGIT_BASE && target < CITY_DIGIT_BASE + 10)
        {
            if (length + 1 < sizeof(current->city_digits))
            {
                current->city_digits[length] = '0' + target - CITY_DIGIT_BASE;
                current->city_digits[length + 1] = '\0';
                text_update(current->city_input, current->city_digits);
            }
            return true;
        }
        if (target == CITY_ERASE)
        {
            if (length) current->city_digits[length - 1] = '\0';
            text_update(current->city_input, current->city_digits);
            return true;
        }
        if (target == CITY_CONFIRM)
        {
            if (!length) popup_open("请输入数字城市ID", NULL);
            else weather_action_start(WEATHER_JOB_CITY);
            return true;
        }
    }

    return false;
}

void city_create(lv_obj_t *screen)
{
    page_title_create(screen, "天气城市ID", ui_font_title());
    lv_obj_t *quiet = panel_create(screen, (WIDTH - 348) / 2, 296, 348, 348);
    lv_obj_set_style_border_width(quiet, 0, 0);
    lv_obj_set_style_radius(quiet, 0, 0);
    lv_obj_t *qr = lv_qrcode_create(quiet);
    epd_obj_init(qr);
    lv_qrcode_set_size(qr, 300);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_set_quiet_zone(qr, true);
    lv_obj_center(qr);
    if (lv_qrcode_update(qr, WEATHER_CITY_LOOKUP_URL, strlen(WEATHER_CITY_LOOKUP_URL)) != LV_RESULT_OK)
        centered_label(screen, "二维码生成失败", MARGIN, 420, CONTENT_WIDTH, ui_font_body());
    /* Make the indexed canvas visible to EPIC before it is drawn. */
    epd_app_clean_draw_buffer(lv_canvas_get_draw_buf(qr));
    centered_label(screen, "扫码查看城市ID", MARGIN, 696, CONTENT_WIDTH, ui_font_body());
    button_create(screen, 112, 800, 460, 80, "我已获取城市ID", UI_PAGE_CITY_INPUT);
}

void city_input_create(lv_obj_t *screen)
{
    weather_config_t cfg;
    weather_get_config(&cfg);
    snprintf(current->city_digits, sizeof(current->city_digits), "%s",
             city_draft_valid ? city_draft : cfg.city_id);
    page_title_create(screen, "天气城市ID", ui_font_title());
    centered_label(screen, "请输入数字城市ID", MARGIN, 238, CONTENT_WIDTH, ui_font_body());
    lv_obj_t *input = panel_create(screen, 64, 302, 556, 88);
    current->city_input = centered_label(input, current->city_digits, 16,
        (88 - ui_font_title()->line_height) / 2, 524, ui_font_title());
    /* A static label avoids the textarea cursor's periodic blinking on e-paper. */
    lv_obj_t *keys = panel_create(screen, MARGIN, 724, CONTENT_WIDTH, 436);
    static const char *const labels[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "确认", "退"};
    for (unsigned i = 0; i < 12; ++i)
    {
        int target = i < 9 ? CITY_DIGIT_BASE + (int)i + 1 :
                     i == 9 ? CITY_DIGIT_BASE : i == 10 ? CITY_CONFIRM : CITY_ERASE;
        button_create(keys, 18 + (i % 3) * 198, 18 + (i / 3) * 102, 186, 88, labels[i], target);
    }
}

void weather_settings_render(void)
{
    weather_config_t cfg;
    char text[128];
    weather_get_config(&cfg);
    snprintf(text, sizeof(text), "天气配置：%s   城市：%s", cfg.api_key[0] ? "已导入" : "未导入",
             cfg.city[0] ? cfg.city : "未设置");
    text_update(current->weather_config_label, text);
}

void weather_settings_create(lv_obj_t *screen)
{
    page_title_create(screen, "天气设置", ui_font_title());
    current->weather_config_label = centered_label(screen, "", MARGIN, 246, CONTENT_WIDTH, ui_font_small());
    weather_settings_render();
    button_create(screen, MARGIN, 348, CONTENT_WIDTH, 84, "从 TF 卡导入天气配置", WEATHER_IMPORT);
    button_create(screen, MARGIN, 464, CONTENT_WIDTH, 84, "天气城市ID", UI_PAGE_CITY);
    button_create(screen, MARGIN, 580, CONTENT_WIDTH, 84, "查询历史", UI_PAGE_CITY_HISTORY);
    centered_label(screen, "配置文件：qweather.json", MARGIN, 724, CONTENT_WIDTH, ui_font_small());
}
