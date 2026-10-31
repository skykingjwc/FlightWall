#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "hardware/clocks.h"
#include "pico/cyw43_arch.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "font8_data.hpp"
#include "aircraft_data.h"
#include "flightwall_settings.h"
#include "hub75.hpp"
#include "ota_server.h"
#include "update_client.h"
#include "wifi_provisioning.h"

constexpr char APP_NAME[] = "CHATGPT FLIGHTWALL";
constexpr char APP_VERSION[] = FLIGHTWALL_VERSION;

constexpr Hub75Config panel_cfg{
    .panel = {
        .matrix_panel_width = 64,
        .matrix_panel_height = 32,
        // Four panels in a 2x2 serpentine chain:
        // controller -> bottom-right -> bottom-left -> top-left -> top-right.
        .chain_rows = 2,
        .chain_cols = 2,
        .chain_mode = Hub75ChainMode::SERPENTINE,
        .panel_kind = RowMapping::Standard,
        .panel_chip = Hub75PanelChip::GENERIC,
        .inverted_stb = false,
        .sm_clockdiv_factor = 1.0f,
        .base_latch_ns = 180,
        .base_addr_ns = 260,
    },
    .screen = {.rotation = Hub75Rotation::DEG_0},
    .pins = {
        .data_base_pin = 2,
        .data_n_pins = 6,
        .rowsel_base_pin = 8,
        .rowsel_n_pins = 4,
        .clk_pin = 13,
        .strobe_pin = 14,
        .oen_pin = 15,
    },
    .color = {
        .bitplanes = 8,
        .separate_cie_channels = false,
        .balanced_light_output = true,
        .ccm_rg_shift = 6,
        .ccm_gb_shift = 7,
    },
    .frame_rate_debug = false,
};

using Panel = Hub75Driver<panel_cfg>;
static Panel driver;

constexpr int WIDTH = Panel::SCREEN_WIDTH;
constexpr int HEIGHT = Panel::SCREEN_HEIGHT;
constexpr int PHYSICAL_PANEL_WIDTH = 64;
constexpr int PHYSICAL_PANEL_HEIGHT = 32;
static uint8_t framebuffer[WIDTH * HEIGHT * 3];
static uint32_t applied_settings_generation;
static size_t aircraft_page;

struct Color { uint8_t r, g, b; };
constexpr Color BLACK{0, 0, 0};
constexpr Color AMBER{255, 175, 0};
constexpr Color WHITE{255, 255, 255};
constexpr Color CYAN{0, 210, 255};
constexpr Color GREEN{0, 255, 80};
constexpr Color YELLOW{255, 255, 0};
constexpr Color RED{255, 0, 0};
constexpr Color RADAR_GRID{0, 70, 90};
constexpr Color RADAR_SWEEP_FAINT{0, 20, 8};
constexpr Color RADAR_SWEEP_DIM{0, 35, 14};
constexpr Color RADAR_SWEEP_MID{0, 65, 24};
constexpr Color RADAR_SWEEP_LEAD{0, 130, 42};
constexpr Color PURPLE{150, 50, 230};
constexpr Color ORANGE{255, 105, 0};
constexpr Color BLUE{30, 105, 255};

void set_pixel(int x, int y, Color color) {
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
    // Preserve the proven quadrant mapping, then rotate the image within each
    // physical 64x32 tile. This corrects the upside-down image without moving
    // any quadrant to a different panel.
    const int panel_x = x / PHYSICAL_PANEL_WIDTH;
    const int panel_y = y / PHYSICAL_PANEL_HEIGHT;
    const int local_x = x % PHYSICAL_PANEL_WIDTH;
    const int local_y = y % PHYSICAL_PANEL_HEIGHT;
    // Hardware validation of v1.5.0 showed that the four upright tiles were
    // globally reversed: left/right and top/bottom. Pre-permute both tile axes
    // while retaining the proven per-tile 180-degree pixel correction.
    const int destination_panel_x = WIDTH / PHYSICAL_PANEL_WIDTH - 1 - panel_x;
    const int destination_panel_y = HEIGHT / PHYSICAL_PANEL_HEIGHT - 1 - panel_y;
    const int rotated_x = destination_panel_x * PHYSICAL_PANEL_WIDTH +
                          (PHYSICAL_PANEL_WIDTH - 1 - local_x);
    const int rotated_y = destination_panel_y * PHYSICAL_PANEL_HEIGHT +
                          (PHYSICAL_PANEL_HEIGHT - 1 - local_y);

    // The first panel in the chain is on the physical bottom row.
    const int chain_y = (rotated_y + PHYSICAL_PANEL_HEIGHT) % HEIGHT;
    const int offset = (chain_y * WIDTH + rotated_x) * 3;
    framebuffer[offset] = color.b;
    framebuffer[offset + 1] = color.g;
    framebuffer[offset + 2] = color.r;
}

void clear(Color color = BLACK) {
    for (int y = 0; y < HEIGHT; ++y)
        for (int x = 0; x < WIDTH; ++x) set_pixel(x, y, color);
}

void filled_rectangle(int x, int y, int width, int height, Color color) {
    for (int py = y; py < y + height; ++py)
        for (int px = x; px < x + width; ++px) set_pixel(px, py, color);
}

void draw_line(int x0, int y0, int x1, int y1, Color color) {
    int dx = std::abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        set_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        const int twice_error = 2 * error;
        if (twice_error >= dy) { error += dy; x0 += sx; }
        if (twice_error <= dx) { error += dx; y0 += sy; }
    }
}

int glyph_width(char character) {
    const unsigned char c = static_cast<unsigned char>(character);
    return (c >= 32 && c <= 127) ? font8.widths[c - 32] : font8.widths['?' - 32];
}

int text_width(const char *text) {
    int width = 0;
    for (size_t i = 0; text[i]; ++i) width += glyph_width(text[i]) + 1;
    return width > 0 ? width - 1 : 0;
}

void draw_character(int x, int y, char character, Color color) {
    unsigned char c = static_cast<unsigned char>(character);
    if (c < 32 || c > 127) c = '?';
    const int index = c - 32;
    const uint8_t *columns = &font8.data[index * font8.max_width];
    for (int column = 0; column < font8.widths[index]; ++column)
        for (int row = 0; row < font8.height; ++row)
            if (columns[column] & (1u << row)) set_pixel(x + column, y + row, color);
}

void draw_text(int x, int y, const char *text, Color color) {
    for (size_t i = 0; text[i]; ++i) {
        draw_character(x, y, text[i], color);
        x += glyph_width(text[i]) + 1;
    }
}

void centered_text(int y, const char *text, Color color) {
    draw_text((WIDTH - text_width(text)) / 2, y, text, color);
}

void present() { driver.update_bgr(framebuffer); }

void show_panel_mapping_test() {
    clear();
    filled_rectangle(63, 0, 2, HEIGHT, RADAR_GRID);
    filled_rectangle(0, 31, WIDTH, 2, RADAR_GRID);
    draw_text((PHYSICAL_PANEL_WIDTH - text_width("TOP LEFT")) / 2,
              12, "TOP LEFT", CYAN);
    draw_text(PHYSICAL_PANEL_WIDTH +
              (PHYSICAL_PANEL_WIDTH - text_width("TOP RIGHT")) / 2,
              12, "TOP RIGHT", GREEN);
    draw_text((PHYSICAL_PANEL_WIDTH - text_width("BTM LEFT")) / 2,
              44, "BTM LEFT", AMBER);
    draw_text(PHYSICAL_PANEL_WIDTH +
              (PHYSICAL_PANEL_WIDTH - text_width("BTM RIGHT")) / 2,
              44, "BTM RIGHT", WHITE);
    present();
    sleep_ms(5000);
}

void show_startup() {
    clear();
    char version_line[32];
    std::snprintf(version_line, sizeof(version_line), "EMBEDDED %s", APP_VERSION);
    centered_text(16, APP_NAME, AMBER);
    centered_text(24, version_line, WHITE);
    centered_text(32, "PICO 2 W", CYAN);
    centered_text(40, "BY JORDAN COBB", GREEN);
    present();
    sleep_ms(3500);
}

extern "C" void show_wifi_status(const char *line1, const char *line2, const char *line3) {
    clear();
    centered_text(17, line1 ? line1 : "WIFI", AMBER);
    centered_text(28, line2 ? line2 : "", CYAN);
    centered_text(39, line3 ? line3 : "", GREEN);
    present();
}

void ota_checkpoint() {
    uint32_t generation = flightwall_settings_generation();
    if (generation != applied_settings_generation) {
        const flightwall_settings_t *settings = flightwall_settings_get();
        driver.setBasisBrightness(settings->panel_output_basis);
        driver.setIntensity(settings->brightness_percent / 100.0f);
        applied_settings_generation = generation;
    }
    if (!flightwall_ota_update_ready()) (void)flightwall_update_process_requests();
    if (!flightwall_ota_update_ready()) (void)flightwall_update_check_due();
    if (!flightwall_ota_update_ready()) return;
    clear();
    centered_text(16, "UPDATE RECEIVED", AMBER);
    centered_text(27, "RESTARTING", CYAN);
    centered_text(38, "ROLLBACK PROTECTED", GREEN);
    present();
    sleep_ms(750);
    flightwall_ota_reboot_to_update();
}

void normal_screen_delay() {
    const uint32_t duration_ms = static_cast<uint32_t>(flightwall_settings_get()->display_seconds) * 1000u;
    for (uint32_t elapsed = 0; elapsed < duration_ms; elapsed += 100u) {
        if (flightwall_emergency_test_active() || flightwall_load_test_active()) break;
        sleep_ms(std::min<uint32_t>(100, duration_ms - elapsed));
    }
}

void draw_trend_arrow(int x, int y, bool climbing, Color color) {
    if (climbing) {
        set_pixel(x + 1, y, color);
        set_pixel(x, y + 1, color); set_pixel(x + 1, y + 1, color); set_pixel(x + 2, y + 1, color);
        for (int py = 2; py < 6; ++py) set_pixel(x + 1, y + py, color);
    } else {
        for (int py = 0; py < 4; ++py) set_pixel(x + 1, y + py, color);
        set_pixel(x, y + 4, color); set_pixel(x + 1, y + 4, color); set_pixel(x + 2, y + 4, color);
        set_pixel(x + 1, y + 5, color);
    }
}

void compact_altitude(const flightwall_aircraft_t &plane, char *text, size_t size) {
    if (plane.altitude_ft == INT32_MIN) std::snprintf(text, size, "---");
    else if (plane.altitude_ft < 0) std::snprintf(text, size, "GND");
    else std::snprintf(text, size, "%ld", static_cast<long>((plane.altitude_ft + 50) / 100));
}

void full_altitude(const flightwall_aircraft_t &plane, char *text, size_t size) {
    if (plane.altitude_ft == INT32_MIN) std::snprintf(text, size, "---");
    else if (plane.altitude_ft < 0) std::snprintf(text, size, "GND");
    else std::snprintf(text, size, "%ld", static_cast<long>(plane.altitude_ft));
}

const char *cardinal_direction(uint16_t track) {
    static constexpr const char *directions[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return directions[((track + 22) / 45) & 7];
}

void draw_circle(int cx, int cy, int radius, Color color);

bool callsign_prefix(const char *id, const char *prefix) {
    return id && std::strncmp(id, prefix, 3) == 0;
}

void draw_delta_mark(int x, int y) {
    // Final low-resolution Delta widget: sharp navy upper triangle with a
    // centered chevron cutout, plus a compact red triangle pointing upward.
    constexpr Color DELTA_NAVY{10, 20, 105};

    // Upper navy widget, 29 px wide by 14 px tall.
    for (int row = 0; row < 14; ++row) {
        const int outer_half = row;
        const int cut_half = row < 7 ? -1 : row - 6;
        for (int px = 14 - outer_half; px <= 14 + outer_half; ++px) {
            const bool in_cutout =
                cut_half >= 0 && px >= 14 - cut_half && px <= 14 + cut_half;
            if (!in_cutout) set_pixel(x + px, y + row, DELTA_NAVY);
        }
    }

    // Lower red wedge points UP: narrow apex nearest the blue widget and
    // progressively wider rows toward the bottom.
    for (int row = 0; row < 6; ++row) {
        const int half = 2 + row * 2;
        for (int px = 14 - half; px <= 14 + half; ++px)
            set_pixel(x + px, y + 15 + row, RED);
    }
}

void draw_united_mark(int x, int y) {
    // Enlarge the globe substantially and simplify the latitude/longitude grid.
    const int cx = x + 10, cy = y + 10;
    draw_circle(cx, cy, 9, BLUE);
    draw_line(cx - 8, cy, cx + 8, cy, BLUE);
    draw_line(cx - 7, cy - 4, cx + 7, cy - 4, BLUE);
    draw_line(cx - 7, cy + 4, cx + 7, cy + 4, BLUE);
    draw_line(cx, cy - 8, cx, cy + 8, BLUE);
    draw_line(cx - 4, cy - 7, cx - 4, cy + 7, BLUE);
    draw_line(cx + 4, cy - 7, cx + 4, cy + 7, BLUE);
    draw_line(cx - 7, cy - 7, cx + 7, cy + 7, CYAN);
    draw_line(cx - 7, cy + 7, cx + 7, cy - 7, CYAN);
}

void draw_american_mark(int x, int y) {
    // Broad red/white/blue feather strokes modeled after the current eagle mark.
    for (int i = 0; i < 3; ++i) {
        draw_line(x + 1 + i, y + 18, x + 15 + i, y + 2, BLUE);
        draw_line(x + 8 + i, y + 18, x + 22 + i, y + 2, WHITE);
        draw_line(x + 15 + i, y + 18, x + 29 + i, y + 4, RED);
    }
}

void draw_ups_mark(int x, int y) {
    // Large shield outline with the familiar UPS lettering centered inside.
    draw_line(x + 3, y + 1, x + 19, y + 1, AMBER);
    draw_line(x + 2, y + 2, x + 2, y + 12, AMBER);
    draw_line(x + 20, y + 2, x + 20, y + 12, AMBER);
    draw_line(x + 2, y + 12, x + 11, y + 20, AMBER);
    draw_line(x + 20, y + 12, x + 11, y + 20, AMBER);
    draw_text(x + 4, y + 6, "UPS", AMBER);
}

void draw_fedex_mark(int x, int y) {
    // FedEx is fundamentally a wordmark, so use the full available width.
    draw_text(x, y + 6, "FED", PURPLE);
    draw_text(x + text_width("FED"), y + 6, "EX", ORANGE);
    draw_line(x + 1, y + 16, x + 30, y + 16, WHITE);
}

void draw_skywest_mark(int x, int y) {
    // The SkyWest logo is also primarily a wordmark; spell it out rather than
    // using the old abbreviated SKY/W treatment.
    draw_text(x, y + 6, "SKY", BLUE);
    draw_text(x + text_width("SKY"), y + 6, "WEST", CYAN);
    draw_line(x + 2, y + 16, x + 40, y + 16, BLUE);
}

void draw_republic_mark(int x, int y) {
    // Simplified large star/wing mark; the larger footprint reads better than
    // the previous tiny star on a 64-pixel-high display.
    const int cx = x + 11, cy = y + 10;
    draw_line(cx, y + 1, cx + 3, cy - 3, RED);
    draw_line(cx + 3, cy - 3, x + 21, cy - 3, RED);
    draw_line(x + 21, cy - 3, cx + 5, cy + 1, RED);
    draw_line(cx + 5, cy + 1, cx + 8, y + 19, RED);
    draw_line(cx + 8, y + 19, cx, cy + 5, RED);
    draw_line(cx, cy + 5, x + 3, y + 19, RED);
    draw_line(x + 3, y + 19, cx - 5, cy + 1, RED);
    draw_line(cx - 5, cy + 1, x + 1, cy - 3, RED);
    draw_line(x + 1, cy - 3, cx - 3, cy - 3, RED);
    draw_line(cx - 3, cy - 3, cx, y + 1, RED);
}

void draw_frontier_mark(int x, int y) {
    // Frontier's compact F9 mark survives the matrix better than the full word.
    draw_text(x + 5, y + 6, "F9", GREEN);
    draw_line(x + 2, y + 3, x + 2, y + 18, GREEN);
    draw_line(x + 2, y + 3, x + 25, y + 3, GREEN);
    draw_line(x + 2, y + 10, x + 18, y + 10, GREEN);
}

bool draw_airline_logo(int x, int y, const char *id) {
    // All marks are laid out for the same roughly 50x22 pixel identity box.
    if (callsign_prefix(id, "DAL")) draw_delta_mark(x + 10, y);
    else if (callsign_prefix(id, "UAL")) draw_united_mark(x + 15, y);
    else if (callsign_prefix(id, "AAL")) draw_american_mark(x + 8, y);
    else if (callsign_prefix(id, "UPS")) draw_ups_mark(x + 14, y);
    else if (callsign_prefix(id, "FDX")) draw_fedex_mark(x + 9, y + 1);
    else if (callsign_prefix(id, "SKW")) draw_skywest_mark(x + 4, y + 1);
    else if (callsign_prefix(id, "RPA")) draw_republic_mark(x + 14, y);
    else if (callsign_prefix(id, "FFT")) draw_frontier_mark(x + 11, y);
    else return false;
    return true;
}

void show_aircraft_list() {
    clear();
    const flightwall_settings_t *settings = flightwall_settings_get();
    draw_text(0, 0, flightwall_aircraft_is_stale(settings->refresh_seconds) ? "STALE" : "AIRCRAFT", AMBER);
    draw_text(48, 0, "ALT", AMBER);
    draw_text(72, 0, "NM", AMBER);
    draw_text(92, 0, "D", AMBER);
    draw_text(104, 0, "TYPE", AMBER);

    size_t count = 0;
    const flightwall_aircraft_t *planes = flightwall_aircraft_list(&count);
    if (count == 0) {
        centered_text(27, flightwall_aircraft_has_success() ? "NO TRAFFIC IN RANGE" : flightwall_aircraft_status(),
                      flightwall_aircraft_has_success() ? WHITE : AMBER);
        present();
        normal_screen_delay();
        return;
    }

    constexpr size_t rows_per_page = 7;
    const size_t pages = (count + rows_per_page - 1) / rows_per_page;
    if (aircraft_page >= pages) aircraft_page = 0;
    const size_t start = aircraft_page * rows_per_page;
    char number[8], altitude[8];
    for (size_t row = 0; row < rows_per_page && start + row < count; ++row) {
        const flightwall_aircraft_t &plane = planes[start + row];
        const int y = 8 + static_cast<int>(row) * 8;
        draw_text(0, y, plane.id, plane.emergency ? RED : (plane.watched ? YELLOW : AMBER));
        compact_altitude(plane, altitude, sizeof(altitude));
        draw_text(48, y, altitude, CYAN);
        if (plane.vertical_rate_fpm > 250 || plane.vertical_rate_fpm < -250)
            draw_trend_arrow(67, y, plane.vertical_rate_fpm > 0, CYAN);
        std::snprintf(number, sizeof(number), "%u", static_cast<unsigned>(std::min(999.0f, std::round(plane.distance_nm))));
        draw_text(72, y, number, GREEN);
        draw_text(92, y, cardinal_direction(plane.track_deg), CYAN);
        draw_text(104, y, plane.type[0] ? plane.type : "----", WHITE);
    }
    aircraft_page = (aircraft_page + 1) % pages;
    present();
    normal_screen_delay();
}

void show_all_aircraft_pages() {
    size_t count = 0;
    flightwall_aircraft_list(&count);
    constexpr size_t rows_per_page = 7;
    const size_t pages = count ? (count + rows_per_page - 1) / rows_per_page : 1;

    for (size_t page = 0; page < pages; ++page) {
        show_aircraft_list();
        ota_checkpoint();
        if (flightwall_emergency_test_active() || flightwall_load_test_active()) break;
    }
}

void show_spotlight() {
    clear();
    const flightwall_aircraft_t *plane = flightwall_aircraft_nearest();
    if (!plane) {
        centered_text(18, "NEAREST AIRCRAFT", AMBER);
        centered_text(34, "NO TRAFFIC", WHITE);
        present();
        normal_screen_delay();
        return;
    }
    char altitude[12], line[40];
    full_altitude(*plane, altitude, sizeof(altitude));

    const bool stale = flightwall_aircraft_is_stale(flightwall_settings_get()->refresh_seconds);
    draw_text(1, 0, "NEAREST AIRCRAFT", stale ? RED : AMBER);

    // Identity band above the horizontal panel seam: text on the left, with
    // the airline symbol using the full upper-right area.
    draw_text(1, 10, plane->id, plane->watched ? YELLOW : AMBER);
    std::snprintf(line, sizeof(line), "%s%s%s", plane->type[0] ? plane->type : "TYPE ----",
                  plane->registration[0] ? " " : "",
                  plane->registration[0] ? plane->registration : "");
    draw_text(1, 20, line, CYAN);
    draw_airline_logo(78, 8, plane->id);

    // Balance the flight data across both bottom panels. The y positions leave
    // two clear rows below the seam and one clear row at the bottom edge.
    if (plane->altitude_ft >= 0)
        std::snprintf(line, sizeof(line), "%s FT", altitude);
    else
        std::snprintf(line, sizeof(line), "%s", altitude);
    draw_text(1, 34, line, CYAN);
    if (plane->vertical_rate_fpm > 250 || plane->vertical_rate_fpm < -250)
        draw_trend_arrow(std::min(59, text_width(line) + 4), 35,
                         plane->vertical_rate_fpm > 0, CYAN);

    std::snprintf(line, sizeof(line), "%u NM %s",
                  static_cast<unsigned>(std::min(999.0f, std::round(plane->distance_nm))),
                  cardinal_direction(plane->bearing_deg));
    draw_text(67, 34, line, GREEN);

    std::snprintf(line, sizeof(line), "%d FPM", plane->vertical_rate_fpm);
    draw_text(1, 45, line, CYAN);
    if (plane->vertical_rate_fpm > 250 || plane->vertical_rate_fpm < -250)
        draw_trend_arrow(std::min(59, text_width(line) + 4), 45,
                         plane->vertical_rate_fpm > 0, CYAN);
    std::snprintf(line, sizeof(line), "%u KT", plane->speed_kt);
    draw_text(67, 45, line, GREEN);

    std::snprintf(line, sizeof(line), "TRK %03u", plane->track_deg);
    draw_text(1, 55, line, CYAN);
    std::snprintf(line, sizeof(line), "SQK %s", plane->squawk[0] ? plane->squawk : "----");
    draw_text(67, 55, line, plane->watched ? YELLOW : WHITE);
    present();
    normal_screen_delay();
}

void draw_circle(int cx, int cy, int radius, Color color) {
    int x = radius, y = 0, error = 1 - radius;
    while (x >= y) {
        set_pixel(cx + x, cy + y, color); set_pixel(cx + y, cy + x, color);
        set_pixel(cx - y, cy + x, color); set_pixel(cx - x, cy + y, color);
        set_pixel(cx - x, cy - y, color); set_pixel(cx - y, cy - x, color);
        set_pixel(cx + y, cy - x, color); set_pixel(cx + x, cy - y, color);
        ++y;
        if (error < 0) error += 2 * y + 1;
        else { --x; error += 2 * (y - x + 1); }
    }
}

void radar_dot(int cx, int cy, int radius, float bearing, float distance_fraction, Color color) {
    constexpr float pi = 3.14159265f;
    const float angle = (bearing - 90.0f) * pi / 180.0f;
    const int x = static_cast<int>(std::lround(cx + std::cos(angle) * radius * distance_fraction));
    const int y = static_cast<int>(std::lround(cy + std::sin(angle) * radius * distance_fraction));
    set_pixel(x, y, color);
}

void mark_radar_grid_pixel(int x, int y) {
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;
    set_pixel(x, y, RADAR_GRID);
}

void draw_radar_grid_circle(int cx, int cy, int radius) {
    int x = radius, y = 0, error = 1 - radius;
    while (x >= y) {
        mark_radar_grid_pixel(cx + x, cy + y); mark_radar_grid_pixel(cx + y, cy + x);
        mark_radar_grid_pixel(cx - y, cy + x); mark_radar_grid_pixel(cx - x, cy + y);
        mark_radar_grid_pixel(cx - x, cy - y); mark_radar_grid_pixel(cx - y, cy - x);
        mark_radar_grid_pixel(cx + y, cy - x); mark_radar_grid_pixel(cx + x, cy - y);
        ++y;
        if (error < 0) error += 2 * y + 1;
        else { --x; error += 2 * (y - x + 1); }
    }
}

Color radar_sweep_color(float fraction, unsigned brightness_percent) {
    const float lead_green = 255.0f * static_cast<float>(brightness_percent) / 100.0f;
    const float lead_blue = lead_green * (42.0f / 130.0f);
    return Color{
        0,
        static_cast<uint8_t>(std::clamp(lead_green * fraction, 0.0f, 255.0f)),
        static_cast<uint8_t>(std::clamp(lead_blue * fraction, 0.0f, 255.0f))
    };
}

void draw_radar_sweep_wedge(int cx, int cy, int radius, float head_angle) {
    constexpr float pi = 3.14159265f;
    const flightwall_settings_t *settings = flightwall_settings_get();
    const float tail_width = static_cast<float>(settings->radar_sweep_tail_degrees);
    const unsigned brightness = settings->radar_sweep_brightness_percent;
    const int radius_squared = radius * radius;

    for (int y = cy - radius; y <= cy + radius; ++y) {
        for (int x = cx - radius; x <= cx + radius; ++x) {
            const int dx = x - cx;
            const int dy = y - cy;
            if (dx * dx + dy * dy > radius_squared) continue;

            float angle = std::atan2(static_cast<float>(dy), static_cast<float>(dx)) * 180.0f / pi + 90.0f;
            if (angle < 0.0f) angle += 360.0f;

            float behind = head_angle - angle;
            while (behind < 0.0f) behind += 360.0f;
            while (behind >= 360.0f) behind -= 360.0f;
            if (behind > tail_width) continue;

            const float position = tail_width > 0.0f ? behind / tail_width : 0.0f;
            float intensity;
            if (position < 0.12f) intensity = 1.0f;
            else if (position < 0.36f) intensity = 0.50f;
            else if (position < 0.68f) intensity = 0.27f;
            else intensity = 0.15f;

            set_pixel(x, y, radar_sweep_color(intensity, brightness));
        }
    }
}

void draw_summary_frame(float sweep_angle) {
    clear();
    constexpr int cx = 30, cy = 31, radar_radius = 28;

    // Paint a continuous trailing wedge first. The fixed blue grid is drawn
    // afterward so its circles and crosshairs stay solid instead of punching
    // holes through the moving sweep.
    draw_radar_sweep_wedge(cx, cy, radar_radius, sweep_angle);

    draw_radar_grid_circle(cx, cy, radar_radius);
    draw_radar_grid_circle(cx, cy, radar_radius / 2);
    for (int y = cy - radar_radius; y <= cy + radar_radius; ++y) mark_radar_grid_pixel(cx, y);
    for (int x = cx - radar_radius; x <= cx + radar_radius; ++x) mark_radar_grid_pixel(x, cy);

    set_pixel(cx, cy, GREEN); set_pixel(cx - 1, cy, GREEN); set_pixel(cx + 1, cy, GREEN);
    set_pixel(cx, cy - 1, GREEN); set_pixel(cx, cy + 1, GREEN);
    size_t count = 0;
    const flightwall_aircraft_t *planes = flightwall_aircraft_list(&count);
    const float radius = std::max(1u, static_cast<unsigned>(flightwall_settings_get()->radius_nm));
    for (size_t i = 0; i < count; ++i)
        radar_dot(cx, cy, radar_radius, planes[i].bearing_deg,
                  std::min(1.0f, planes[i].distance_nm / radius),
                  planes[i].emergency ? RED : (planes[i].watched ? YELLOW : CYAN));

    char line[24];
    std::snprintf(line, sizeof(line), "NOW %u", static_cast<unsigned>(count));
    draw_text(64, 2, line, WHITE);
    std::snprintf(line, sizeof(line), "1HR %lu", static_cast<unsigned long>(flightwall_aircraft_unique_hour()));
    draw_text(64, 14, line, CYAN);
    std::snprintf(line, sizeof(line), "24HR %lu", static_cast<unsigned long>(flightwall_aircraft_unique_day()));
    draw_text(64, 26, line, AMBER);
    const flightwall_aircraft_t *nearest = flightwall_aircraft_nearest();
    if (nearest) std::snprintf(line, sizeof(line), "NEAR %uNM",
                               static_cast<unsigned>(std::min(999.0f, std::round(nearest->distance_nm))));
    else std::snprintf(line, sizeof(line), "NEAR ---");
    draw_text(64, 38, line, GREEN);
    std::snprintf(line, sizeof(line), "RANGE %uNM", flightwall_settings_get()->radius_nm);
    draw_text(64, 50, line, WHITE);
}

void show_summary() {
    constexpr uint32_t frame_ms = 75;
    constexpr float degrees_per_second = 60.0f;
    const uint32_t duration_ms = static_cast<uint32_t>(flightwall_settings_get()->display_seconds) * 1000u;
    const uint32_t started_ms = to_ms_since_boot(get_absolute_time());

    while (true) {
        if (flightwall_emergency_test_active() || flightwall_load_test_active()) break;
        const uint32_t now = to_ms_since_boot(get_absolute_time());
        const uint32_t elapsed = now - started_ms;
        if (elapsed >= duration_ms) break;

        const float sweep_angle = std::fmod(
            static_cast<float>(now) * degrees_per_second / 1000.0f, 360.0f);
        draw_summary_frame(sweep_angle);
        present();
        sleep_ms(std::min<uint32_t>(frame_ms, duration_ms - elapsed));
    }
}

const char *emergency_description(const flightwall_aircraft_t &plane) {
    if (std::strcmp(plane.squawk, "7500") == 0) return "HIJACK";
    if (std::strcmp(plane.squawk, "7600") == 0) return "RADIO";
    return "EMERGENCY";
}

void show_emergency(const flightwall_aircraft_t &plane, bool test_mode = false) {
    const char *label = "*** EMERGENCY ***";
    const int label_x = (WIDTH - text_width(label)) / 2;
    char altitude[12], line[40];
    full_altitude(plane, altitude, sizeof(altitude));
    for (int frame = 0; frame < 12; ++frame) {
        if (test_mode && !flightwall_emergency_test_active()) break;
        clear();
        filled_rectangle(0, 0, WIDTH, 2, RED);
        filled_rectangle(0, HEIGHT - 2, WIDTH, 2, RED);
        filled_rectangle(0, 5, label_x - 3, 3, RED);
        filled_rectangle(label_x + text_width(label) + 3, 5,
                         WIDTH - label_x - text_width(label) - 3, 3, RED);
        if ((frame % 2) == 0) draw_text(label_x, 3, label, RED);
        centered_text(16, plane.id, RED);
        std::snprintf(line, sizeof(line), "SQK %s %s", plane.squawk[0] ? plane.squawk : "----",
                      emergency_description(plane));
        centered_text(28, line, RED);
        std::snprintf(line, sizeof(line), "ALT %s  %uNM", altitude,
                      static_cast<unsigned>(std::min(999.0f, std::round(plane.distance_nm))));
        centered_text(40, line, RED);
        std::snprintf(line, sizeof(line), "GS %uKT  TRK %03u", plane.speed_kt, plane.track_deg);
        centered_text(52, line, RED);
        present();
        sleep_ms(500);
    }
}

void show_stale_data() {
    clear();
    centered_text(15, "AIRCRAFT DATA", AMBER);
    centered_text(27, "STALE", RED);
    centered_text(39, flightwall_aircraft_status(), WHITE);
    centered_text(51, "RETRYING", CYAN);
    present();
    normal_screen_delay();
}

flightwall_aircraft_t emergency_test_aircraft() {
    flightwall_aircraft_t plane{};
    std::snprintf(plane.id, sizeof(plane.id), "TEST7700");
    std::snprintf(plane.type, sizeof(plane.type), "TEST");
    std::snprintf(plane.squawk, sizeof(plane.squawk), "7700");
    plane.altitude_ft = 4300;
    plane.speed_kt = 210;
    plane.distance_nm = 12.0f;
    plane.emergency = true;
    return plane;
}

void show_load_test() {
    driver.setIntensity(1.0f);
    clear(WHITE);
    present();
    for (int tenth = 0; tenth < 100 && flightwall_load_test_active(); ++tenth) {
        if (flightwall_aircraft_emergency()) break;
        sleep_ms(100);
    }
    flightwall_load_test_complete();
    driver.setIntensity(flightwall_settings_get()->brightness_percent / 100.0f);
    clear();
    present();
}

void core1_entry() {
    multicore_lockout_victim_init();
    driver.create();
    driver.start();
    while (true) tight_loop_contents();
}

int main() {
    set_sys_clock_khz(150000, true);
    stdio_init_all();
    flightwall_ota_confirm_running_image();

    // The Pico W radio and HUB75 engine both consume PIO/DMA resources. Claim
    // the radio resources first; the HUB75 allocator is designed to route
    // itself around an existing CYW43 allocation.
    const int wifi_init_result = cyw43_arch_init();
    flightwall_settings_load();
    multicore_reset_core1();
    multicore_launch_core1(core1_entry);
    sleep_ms(250);

    const flightwall_settings_t *settings = flightwall_settings_get();
    driver.setBasisBrightness(settings->panel_output_basis);
    driver.setIntensity(settings->brightness_percent / 100.0f);
    applied_settings_generation = flightwall_settings_generation();

    show_panel_mapping_test();
    show_startup();
    if (wifi_init_result != PICO_OK) {
        show_wifi_status("WIFI INIT ERROR", "RADIO NOT READY", "USB RECOVERY READY");
        while (true) sleep_ms(1000);
    }
    if (flightwall_wifi_connect_or_provision(show_wifi_status) != 0) {
        show_wifi_status("WIFI ERROR", "RESTART DEVICE", "USB RECOVERY READY");
        while (true) sleep_ms(1000);
    }
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, true);
    if (!flightwall_ota_start()) {
        show_wifi_status("OTA ERROR", flightwall_wifi_ip_address(), "DISPLAY STILL WORKS");
        sleep_ms(3000);
    } else {
        show_wifi_status("FLIGHTWALL ONLINE", flightwall_wifi_ip_address(), "WEB SETTINGS READY");
        sleep_ms(6000);
    }

    // Diagnostic build: do not perform the automatic GitHub HTTPS check at
    // startup. The web UI can still request update checks later through the
    // normal queued update path. This isolates the ADS-B TLS connection from
    // any preceding GitHub TLS activity.
    flightwall_update_init();

    flightwall_aircraft_init();
    show_wifi_status("FETCHING AIRCRAFT", "DATA FROM ADSB.FI", "PLEASE WAIT");
    flightwall_aircraft_refresh();

    // GitHub automatic update checks remain disabled while the updater is
    // moved away from GitHub's RSA TLS transport. ADS-B uses the proven
    // ECDSA-only mbedTLS configuration.
    while (true) {
        const flightwall_settings_t *current_settings = flightwall_settings_get();
        if (flightwall_aircraft_refresh_due(current_settings->refresh_seconds))
            flightwall_aircraft_refresh();

        const bool data_stale = flightwall_aircraft_is_stale(current_settings->refresh_seconds);
        if (!data_stale) {
            if (const flightwall_aircraft_t *emergency = flightwall_aircraft_emergency()) {
                show_emergency(*emergency);
                ota_checkpoint();
                continue;
            }
        }

        if (flightwall_emergency_test_active()) {
            const flightwall_aircraft_t test_plane = emergency_test_aircraft();
            show_emergency(test_plane, true);
            ota_checkpoint();
            continue;
        }

        if (flightwall_load_test_active()) {
            show_load_test();
            ota_checkpoint();
            continue;
        }

        if (data_stale) {
            show_stale_data();
            ota_checkpoint();
            continue;
        }

        show_all_aircraft_pages();
        if (flightwall_emergency_test_active() || flightwall_load_test_active()) continue;
        current_settings = flightwall_settings_get();
        if (flightwall_aircraft_refresh_due(current_settings->refresh_seconds))
            flightwall_aircraft_refresh();
        if (flightwall_aircraft_is_stale(current_settings->refresh_seconds)) continue;
        show_spotlight();
        ota_checkpoint();
        if (flightwall_emergency_test_active() || flightwall_load_test_active()) continue;
        current_settings = flightwall_settings_get();
        if (flightwall_aircraft_refresh_due(current_settings->refresh_seconds))
            flightwall_aircraft_refresh();
        if (flightwall_aircraft_is_stale(current_settings->refresh_seconds)) continue;
        show_summary();
        ota_checkpoint();

        // Force a clean static-frame handoff after the animated radar screen.
        // This prevents any stale sweep/list data from surviving into the next
        // aircraft-list frame if the HUB75 driver misses a boundary update.
        clear();
        present();
        sleep_ms(25);
    }
}
