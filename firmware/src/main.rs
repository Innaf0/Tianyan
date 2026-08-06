#![no_std]
#![no_main]
mod display;
mod irqs;
mod request;
mod sd_card;
mod touch;

use core::cell::RefCell;

use embassy_executor::Spawner;
use embassy_rp::gpio::{Level, Output};
use embassy_rp::peripherals::SPI1;
use embassy_rp::spi::{Blocking, Spi};
use embassy_rp::usb::Driver;
use embassy_time::{Duration, Timer};
use embedded_hal_bus::spi::{NoDelay, RefCellDevice};
use log::{info, warn};
use static_cell::StaticCell;
use {defmt_rtt as _, panic_probe as _};

use crate::irqs::Irqs;

#[embassy_executor::task]
async fn usb_logger_task(driver: Driver<'static, embassy_rp::peripherals::USB>) {
    embassy_usb_logger::run!(1024, log::LevelFilter::Info, driver);
}

#[embassy_executor::main]
async fn main(spawner: Spawner) {
    let p = embassy_rp::init(Default::default());

    // ── USB serial logging ──────────────────────────────────
    let usb_driver = Driver::new(p.USB, Irqs);
    spawner.spawn(usb_logger_task(usb_driver).unwrap());
    // Let the logger task register itself so the first logs aren't dropped.
    embassy_futures::yield_now().await;
    info!("[0] USB logger ready");

    // ── SD card on SPI0 ──────────────────────────────────────
    info!("Waiting 60 s before SD card init...");
    Timer::after(Duration::from_secs(60)).await;
    info!("[1] Initializing SD card on SPI0...");
    let mut sd_handle = sd_card::init_sd_card(
        p.SPI0,   // SPI peripheral
        p.PIN_6,  // SCK  (SD_CLK / SPI0 SCK)
        p.PIN_7,  // MOSI (SD_MOSI / SPI0 TX)
        p.PIN_4,  // MISO (SD_MISO / SPI0 RX)
        p.PIN_21, // CS   (SD_CS)
    );
    info!("[2] SD card initialized");

    info!("[3] Reading CONFIG.TXT...");
    let config = sd_handle.read_config();
    info!(
        "[4] Config loaded: WiFi '{}', player '{}#{}' on '{}'",
        config.wifi_ssid.as_str(),
        config.player_name.as_str(),
        config.player_tag.as_str(),
        config.player_region.as_str(),
    );

    // ─ WiFi (CYW43) ─────────────────────────────────────────
    info!("[5] Initializing WiFi...");
    let ctx = request::init_network(
        spawner,
        config.wifi_ssid.as_str(),
        config.wifi_password.as_str(),
        config.api_key.as_str(),
        config.player_region.as_str(),
        config.player_name.as_str(),
        config.player_tag.as_str(),
        p.PIN_23,
        p.PIN_25,
        p.PIN_24,
        p.PIN_29,
        p.PIO0,
        p.DMA_CH0,
    )
    .await;
    info!("[6] WiFi up");

    // ── Shared SPI1 bus for display + touch ─────────────────
    info!("[7] Setting up SPI1 bus for display + touch...");
    let mut spi_config = embassy_rp::spi::Config::default();
    spi_config.frequency = 64_000_000;

    let spi_bus = Spi::new_blocking(
        p.SPI1, p.PIN_14, // SCK  (DISP_SCK / DISP_T_CLK)
        p.PIN_11, // MOSI (DISP_MOSI / DISP_T_MOSI)
        p.PIN_12, // MISO (DISP_MISO / DISP_T_DO)
        spi_config,
    );

    static SPI_BUS: StaticCell<RefCell<Spi<'static, SPI1, Blocking>>> = StaticCell::new();
    let spi_bus = SPI_BUS.init(RefCell::new(spi_bus));

    // Display CS: GPIO13, Touch CS: GPIO9, Touch IRQ: GPIO8
    let display_spi = RefCellDevice::new(spi_bus, Output::new(p.PIN_13, Level::High), NoDelay);
    let touch_spi = RefCellDevice::new(spi_bus, Output::new(p.PIN_9, Level::High), NoDelay);

    // ── ILI9341 display ─────────────────────────────────────
    info!("[8] Initializing ILI9341 display...");
    let mut display = display::init_display(
        display_spi,
        p.PIN_5,  // DC   (DISP_RS/DC)
        p.PIN_15, // RST  (DISP_RESET)
    );
    info!("[9] Display initialized");

    display::draw_test_circle(&mut display);
    info!("[10] Test circle drawn");

    // ── Touch controller (shares SPI1 with display) ──────────
    let mut touch = touch::Touch::new(touch_spi, p.PIN_8); // IRQ  (DISP_T_IRQ)
    info!("[11] Touch controller initialized");

    info!("[12] Entering main loop");
    let mut match_loaded = false;

    loop {
        // ── Check for touch input ────────────────────────────
        if touch.is_touched() {
            if let Some((x, y)) = touch.read() {
                info!("Touch at ({}, {})", x, y);
            }
        }

        if !match_loaded {
            // ── Fetch match once and update display ───────────────
            info!("[13] Fetching match data...");
            match request::fetch_match(&ctx).await {
                Some(m) => {
                    info!("=== Match ===");
                    info!("Map:         {}", m.map.as_str());
                    info!("Mode:        {}", m.mode.as_str());
                    info!("Region:      {}", m.region.as_str());
                    info!("Started:     {}", m.game_start.as_str());
                    info!("Rounds:      {}", m.rounds_played);
                    info!(
                        "Score:       Red {} - {} Blue",
                        m.red_rounds_won, m.blue_rounds_won,
                    );
                    info!(
                        "Winner:      {}",
                        if m.red_has_won { "Red" } else { "Blue" }
                    );

                    info!("=== Players ===");
                    for player in &m.players {
                        info!(
                            "  [{}] {} #{} | {} | {} | K/D/A: {}/{}/{} | HS: {}",
                            player.team.as_str(),
                            player.name.as_str(),
                            player.tag.as_str(),
                            player.character.as_str(),
                            player.rank.as_str(),
                            player.kills,
                            player.deaths,
                            player.assists,
                            player.headshots,
                        );
                    }

                    display::draw_match(&mut display, &m);
                    match_loaded = true;
                    info!("[14] Match loaded; automatic reload disabled");
                }
                None => {
                    warn!("Failed to fetch match data, retrying in 30 s...");
                    Timer::after(Duration::from_secs(30)).await;
                }
            }
        }

        Timer::after(Duration::from_millis(20)).await;
    }
}
