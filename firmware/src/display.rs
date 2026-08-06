//! ILI9341 TFT display driver via SPI.
//!
//! Initializes the display and provides helpers to draw match info.
//! The SPI bus is shared with the touch controller via `SpiDevice`.

use core::fmt::Write;

use embassy_rp::gpio::{Level, Output, Pin};
use embassy_rp::Peri;
use embedded_graphics::mono_font::iso_8859_1::FONT_6X10;
use embedded_graphics::mono_font::MonoTextStyle;
use embedded_graphics::pixelcolor::Rgb565;
use embedded_graphics::prelude::*;
use embedded_graphics::primitives::{Circle, PrimitiveStyle, Rectangle};
use embedded_graphics::text::Text;
use embedded_hal_1::spi::SpiDevice;
use heapless::String;
use log::info;
use mipidsi::interface::SpiInterface;
use mipidsi::models::ILI9341Rgb565;
use mipidsi::options::ColorOrder;
use mipidsi::Builder;
use static_cell::StaticCell;

use crate::request::MatchInfo;

// --- Public API ---

/// Initialise the ILI9341 display over a shared SPI bus.
///
/// Returns the display object ready for drawing. Pass `&mut display` to
/// [`draw_match`] to render match data.
///
/// `SPI` is the SPI device (e.g., `ExclusiveDevice` or `RefCellDevice`).
pub fn init_display<SPI>(
    spi: SPI,
    dc: Peri<'static, impl Pin>,
    rst: Peri<'static, impl Pin>,
) -> impl DrawTarget<Color = Rgb565>
where
    SPI: SpiDevice + 'static,
{
    static SPI_BUF: StaticCell<[u8; 256]> = StaticCell::new();

    let dc = Output::new(dc, Level::Low);
    let di = SpiInterface::new(spi, dc, SPI_BUF.init([0u8; 256]));

    let rst = Output::new(rst, Level::High);

    let mut display = Builder::new(ILI9341Rgb565, di)
        .reset_pin(rst)
        .display_size(240, 320)
        .color_order(ColorOrder::Bgr)
        .init(&mut embassy_time::Delay)
        .unwrap();

    display.clear(Rgb565::BLACK).unwrap();
    info!("Display initialised");

    display
}

/// Draw a test circle in the centre of the screen.
pub fn draw_test_circle(display: &mut impl DrawTarget<Color = Rgb565>) {
    let circle = Circle::new(Point::new(95, 135), 50)
        .into_styled(PrimitiveStyle::with_stroke(Rgb565::RED, 2));
    let _ = circle.draw(display);
}

/// Draw a full match summary on screen.
pub fn draw_match(display: &mut impl DrawTarget<Color = Rgb565>, m: &MatchInfo) {
    let red = Rgb565::new(28, 6, 8);
    let blue = Rgb565::new(4, 12, 29);

    let _ = display.clear(Rgb565::BLACK);

    let style = MonoTextStyle::new(&FONT_6X10, Rgb565::WHITE);
    let small_style = MonoTextStyle::new(&FONT_6X10, Rgb565::CSS_DARK_GRAY);
    let red_style = MonoTextStyle::new(&FONT_6X10, red);
    let blue_style = MonoTextStyle::new(&FONT_6X10, blue);

    let mut y = 10i32;

    let mut header: String<32> = String::new();
    let _ = core::write!(header, "{} | {}", m.map.as_str(), m.mode.as_str());
    let _ = Text::new(header.as_str(), Point::new(10, y), style).draw(display);
    y += 14;

    let mut score: String<32> = String::new();
    let _ = core::write!(
        score,
        "Red {} - {} Blue",
        m.red_rounds_won,
        m.blue_rounds_won
    );
    let _ = Text::new(score.as_str(), Point::new(10, y), style).draw(display);
    y += 14;

    let winner = if m.red_has_won {
        "Winner: Red"
    } else {
        "Winner: Blue"
    };
    let _ = Text::new(winner, Point::new(10, y), style).draw(display);
    y += 16;

    let _ = Rectangle::new(Point::new(10, y), Size::new(220, 1))
        .into_styled(PrimitiveStyle::with_fill(Rgb565::WHITE))
        .draw(display);
    y += 8;

    for player in &m.players {
        if y > 300 {
            break;
        }

        let team_style = if player.team.as_str() == "Red" {
            red_style
        } else {
            blue_style
        };

        let _ = Text::new(player.name.as_str(), Point::new(10, y), team_style).draw(display);

        let name_chars = player.name.chars().count() as i32;
        let rest_x = 16 + name_chars * 6;
        let mut rest: String<32> = String::new();
        let _ = core::write!(
            rest,
            "({}) {}/{}",
            player.character.as_str(),
            player.kills,
            player.deaths,
        );
        let _ = Text::new(rest.as_str(), Point::new(rest_x, y), small_style).draw(display);

        y += 12;
    }
}
