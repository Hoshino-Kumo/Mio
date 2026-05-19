#include "es8311_audio_codec.h"
#include "config.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>

#define TAG "Es8311AudioCodec"

#ifndef AUDIO_CODEC_INPUT_GAIN_DB
#define AUDIO_CODEC_INPUT_GAIN_DB 30
#endif

#ifndef AUDIO_CODEC_INPUT_I2S_CHANNELS
#define AUDIO_CODEC_INPUT_I2S_CHANNELS 1
#endif

#ifndef AUDIO_CODEC_INPUT_CHANNEL_MASK
#define AUDIO_CODEC_INPUT_CHANNEL_MASK 0
#endif

Es8311AudioCodec::Es8311AudioCodec(void* i2c_master_handle, i2c_port_t i2c_port, int input_sample_rate, int output_sample_rate,
    gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din,
    gpio_num_t pa_pin, uint8_t es8311_addr, bool use_mclk, bool pa_inverted) {
    duplex_ = true; // 是否双工
    input_reference_ = false; // 是否使用参考输入，实现回声消除
    input_channels_ = 1; // 输入通道数
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;
    pa_pin_ = pa_pin;
    pa_inverted_ = pa_inverted;
    input_gain_ = AUDIO_CODEC_INPUT_GAIN_DB;

    assert(input_sample_rate_ == output_sample_rate_);
    CreateDuplexChannels(mclk, bclk, ws, dout, din);

    // Do initialize of related interface: data_if, ctrl_if and gpio_if
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = rx_handle_,
        .tx_handle = tx_handle_,
    };
    data_if_ = audio_codec_new_i2s_data(&i2s_cfg);
    assert(data_if_ != NULL);

    // Output
    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = i2c_port,
        .addr = es8311_addr,
        .bus_handle = i2c_master_handle,
    };
    ctrl_if_ = audio_codec_new_i2c_ctrl(&i2c_cfg);
    assert(ctrl_if_ != NULL);

    gpio_if_ = audio_codec_new_gpio();
    assert(gpio_if_ != NULL);

    es8311_codec_cfg_t es8311_cfg = {};
    es8311_cfg.ctrl_if = ctrl_if_;
    es8311_cfg.gpio_if = gpio_if_;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH;
    es8311_cfg.pa_pin = pa_pin;
    es8311_cfg.use_mclk = use_mclk;
    es8311_cfg.hw_gain.pa_voltage = 5.0;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    es8311_cfg.pa_reverted = pa_inverted_;
    codec_if_ = es8311_codec_new(&es8311_cfg);

    if (codec_if_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create Es8311AudioCodec");
    } else {
        ESP_LOGI(TAG, "Es8311AudioCodec initialized");
    }
}

Es8311AudioCodec::~Es8311AudioCodec() {
    esp_codec_dev_delete(dev_);

    audio_codec_delete_codec_if(codec_if_);
    audio_codec_delete_ctrl_if(ctrl_if_);
    audio_codec_delete_gpio_if(gpio_if_);
    audio_codec_delete_data_if(data_if_);
}

void Es8311AudioCodec::UpdateDeviceState() {
    if ((input_enabled_ || output_enabled_) && dev_ == nullptr) {
        esp_codec_dev_cfg_t dev_cfg = {
            .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
            .codec_if = codec_if_,
            .data_if = data_if_,
        };
        dev_ = esp_codec_dev_new(&dev_cfg);
        assert(dev_ != NULL);

        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = AUDIO_CODEC_INPUT_I2S_CHANNELS,
            .channel_mask = AUDIO_CODEC_INPUT_CHANNEL_MASK,
            .sample_rate = (uint32_t)input_sample_rate_,
            .mclk_multiple = 0,
        };
        ESP_LOGI(TAG, "Open ES8311: sample_rate=%d, i2s_channels=%d, channel_mask=0x%x, input_gain=%.1f dB",
                 input_sample_rate_, AUDIO_CODEC_INPUT_I2S_CHANNELS,
                 AUDIO_CODEC_INPUT_CHANNEL_MASK, input_gain_);
        ESP_ERROR_CHECK(esp_codec_dev_open(dev_, &fs));
        ESP_ERROR_CHECK(esp_codec_dev_set_in_gain(dev_, input_gain_));
        ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(dev_, output_volume_));
    } else if (!input_enabled_ && !output_enabled_ && dev_ != nullptr) {
        esp_codec_dev_close(dev_);
        dev_ = nullptr;
    }
    if (pa_pin_ != GPIO_NUM_NC) {
        int level = output_enabled_ ? 1 : 0;
        gpio_set_level(pa_pin_, pa_inverted_ ? !level : level);
    }
}

void Es8311AudioCodec::LogDiagnostics(const int16_t* data, int samples) {
    int64_t now = esp_timer_get_time();
    if (now - last_diagnostic_log_time_ < 5000000) {
        return;
    }
    last_diagnostic_log_time_ = now;

    int32_t peak = 0;
    for (int i = 0; i < samples; ++i) {
        int32_t value = data[i];
        int32_t abs_value = value < 0 ? -value : value;
        if (abs_value > peak) {
            peak = abs_value;
        }
    }

    int reg_fd = 0;
    int reg_fe = 0;
    int reg_ff = 0;
    int reg_01 = 0;
    int reg_09 = 0;
    int reg_0a = 0;
    int reg_0d = 0;
    int reg_0e = 0;
    int reg_12 = 0;
    int reg_14 = 0;
    int reg_15 = 0;
    int reg_16 = 0;
    int reg_17 = 0;
    int reg_32 = 0;
    int reg_44 = 0;

    esp_codec_dev_read_reg(dev_, 0xfd, &reg_fd);
    esp_codec_dev_read_reg(dev_, 0xfe, &reg_fe);
    esp_codec_dev_read_reg(dev_, 0xff, &reg_ff);
    esp_codec_dev_read_reg(dev_, 0x01, &reg_01);
    esp_codec_dev_read_reg(dev_, 0x09, &reg_09);
    esp_codec_dev_read_reg(dev_, 0x0a, &reg_0a);
    esp_codec_dev_read_reg(dev_, 0x0d, &reg_0d);
    esp_codec_dev_read_reg(dev_, 0x0e, &reg_0e);
    esp_codec_dev_read_reg(dev_, 0x12, &reg_12);
    esp_codec_dev_read_reg(dev_, 0x14, &reg_14);
    esp_codec_dev_read_reg(dev_, 0x15, &reg_15);
    esp_codec_dev_read_reg(dev_, 0x16, &reg_16);
    esp_codec_dev_read_reg(dev_, 0x17, &reg_17);
    esp_codec_dev_read_reg(dev_, 0x32, &reg_32);
    esp_codec_dev_read_reg(dev_, 0x44, &reg_44);

    ESP_LOGI(TAG, "ES8311 diag: id=%02x/%02x/%02x clk01=%02x sdp09=%02x sdp0a=%02x pwr0d=%02x pwr0e=%02x dac12=%02x sys14=%02x adc15=%02x gain16=%02x vol17=%02x dac32=%02x gpio44=%02x raw_peak=%ld first=[%d,%d,%d,%d,%d,%d,%d,%d]",
             reg_fd, reg_fe, reg_ff, reg_01, reg_09, reg_0a, reg_0d, reg_0e, reg_12,
             reg_14, reg_15, reg_16, reg_17, reg_32, reg_44, static_cast<long>(peak),
             samples > 0 ? data[0] : 0, samples > 1 ? data[1] : 0,
             samples > 2 ? data[2] : 0, samples > 3 ? data[3] : 0,
             samples > 4 ? data[4] : 0, samples > 5 ? data[5] : 0,
             samples > 6 ? data[6] : 0, samples > 7 ? data[7] : 0);
}

void Es8311AudioCodec::CreateDuplexChannels(gpio_num_t mclk, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din) {
    assert(input_sample_rate_ == output_sample_rate_);

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
			#ifdef   I2S_HW_VERSION_2    
				.ext_clk_freq_hz = 0,
			#endif
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            #ifdef   I2S_HW_VERSION_2   
                .left_align = true,
                .big_endian = false,
                .bit_order_lsb = false
            #endif
        },
        .gpio_cfg = {
            .mclk = mclk,
            .bclk = bclk,
            .ws = ws,
            .dout = dout,
            .din = din,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
    ESP_LOGI(TAG, "Duplex channels created");
}

void Es8311AudioCodec::SetOutputVolume(int volume) {
    ESP_ERROR_CHECK(esp_codec_dev_set_out_vol(dev_, volume));
    AudioCodec::SetOutputVolume(volume);
}

void Es8311AudioCodec::EnableInput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (codec_if_ == nullptr) {
        return;
    }
    if (enable == input_enabled_) {
        return;
    }
    AudioCodec::EnableInput(enable);
    UpdateDeviceState();
}

void Es8311AudioCodec::EnableOutput(bool enable) {
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    if (codec_if_ == nullptr) {
        return;
    }
    if (enable == output_enabled_) {
        return;
    }
    AudioCodec::EnableOutput(enable);
    UpdateDeviceState();
}

int Es8311AudioCodec::Read(int16_t* dest, int samples) {
    if (!input_enabled_ || dev_ == nullptr) {
        return 0;
    }

    auto ret = esp_codec_dev_read(dev_, (void*)dest, samples * sizeof(int16_t));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read input data: %s", esp_err_to_name(ret));
        return 0;
    }
    LogDiagnostics(dest, samples);
    return samples;
}

int Es8311AudioCodec::Write(const int16_t* data, int samples) {
    if (output_enabled_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_codec_dev_write(dev_, (void*)data, samples * sizeof(int16_t)));
    }
    return samples;
}
