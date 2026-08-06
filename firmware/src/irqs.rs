//! Centralised interrupt bindings for the whole application.
//!
//! All of the IRQ handlers live here so they are defined exactly once.

use embassy_rp::peripherals::{DMA_CH0, PIO0, USB};
use embassy_rp::pio::InterruptHandler;
use embassy_rp::{bind_interrupts, dma, usb};

bind_interrupts!(pub struct Irqs {
    PIO0_IRQ_0 => InterruptHandler<PIO0>;
    USBCTRL_IRQ => usb::InterruptHandler<USB>;
    DMA_IRQ_0 =>
        dma::InterruptHandler<DMA_CH0>;
});
