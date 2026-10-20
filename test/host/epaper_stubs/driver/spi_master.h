#pragma once
#include <cstddef>
#include "gpio.h"
using spi_host_device_t = int;
using spi_device_handle_t = void *;
constexpr int SPI_DMA_CH_AUTO = 0;
struct spi_bus_config_t {
  int miso_io_num, mosi_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num;
  int max_transfer_sz;
};
struct spi_device_interface_config_t {
  int spics_io_num, clock_speed_hz, mode, queue_size;
};
struct spi_transaction_t {
  size_t length;
  const void *tx_buffer;
};
esp_err_t spi_bus_initialize(spi_host_device_t host, const spi_bus_config_t *config, int dma);
esp_err_t spi_bus_add_device(spi_host_device_t host, const spi_device_interface_config_t *config,
                             spi_device_handle_t *device);
esp_err_t spi_bus_free(spi_host_device_t host);
esp_err_t spi_device_polling_transmit(spi_device_handle_t device, spi_transaction_t *transaction);
