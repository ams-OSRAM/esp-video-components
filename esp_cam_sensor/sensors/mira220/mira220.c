/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"

#include "esp_cam_sensor.h"
#include "esp_cam_sensor_detect.h"
#include "mira220_settings.h"
#include "mira220.h"

/*
 * mira220 camera sensor gain control.
 * Note1: The analog gain only has coarse gain, and no fine gain, so in the adjustment of analog gain.
 * Digital gain needs to replace analog fine gain for smooth transition, so as to avoid AGC oscillation.
 * Note2: the analog gain of mira220 will be affected by temperature, it is recommended to increase Dgain first and then Again.
 */


typedef struct {
    uint32_t exposure_val;
    uint32_t exposure_max;
    uint32_t gain_index; // current gain index

    uint32_t vflip_en : 1;
    uint32_t hmirror_en : 1;
} mira220_para_t;

struct mira220_cam {
    mira220_para_t mira220_para;
};

#define mira220_IO_MUX_LOCK(mux)
#define mira220_IO_MUX_UNLOCK(mux)
#define mira220_ENABLE_OUT_XCLK(pin,clk)
#define mira220_DISABLE_OUT_XCLK(pin)

#define EXPOSURE_V4L2_UNIT_US                   100
#define EXPOSURE_V4L2_TO_mira220(v, sf)          \
    ((uint32_t)(((double)v) * (sf)->fps * (sf)->isp_info->isp_v1_info.vts / (1000000 / EXPOSURE_V4L2_UNIT_US) + 0.5))
#define EXPOSURE_mira220_TO_V4L2(v, sf)          \
    ((int32_t)(((double)v) * 1000000 / (sf)->fps / (sf)->isp_info->isp_v1_info.vts / EXPOSURE_V4L2_UNIT_US + 0.5))

#define mira220_VTS_MAX          0x7fff // Max exposure is VTS-6
#define mira220_EXP_MAX_OFFSET   0x06



#define mira220_GROUP_HOLD_START        0x00
#define mira220_GROUP_HOLD_END          0x30
#define mira220_GROUP_HOLD_DELAY_FRAMES 0x01

#define mira220_PID         0x130
#define mira220_SENSOR_NAME "mira220"
#ifndef portTICK_RATE_MS
#define portTICK_RATE_MS portTICK_PERIOD_MS
#endif
#define delay_ms(ms)  vTaskDelay((ms > portTICK_PERIOD_MS ? ms/ portTICK_PERIOD_MS : 1))
#define mira220_SUPPORT_NUM CONFIG_CAMERA_MIRA220_MAX_SUPPORT


static const char *TAG = "mira220";


static const esp_cam_sensor_isp_info_t mira220_isp_info[] = {
    /* For MIPI */
    {
        .isp_v1_info = {
            .version = SENSOR_ISP_INFO_VERSION_DEFAULT,
            .vts = 4100,  // 600 + 3500
            .hts = 1500,
            .pclk = 36900000,
            .bayer_type = ESP_CAM_SENSOR_BAYER_BGGR,
        }
    }
};

static const esp_cam_sensor_format_t mira220_format_info[] = {
    /* For MIPI */
    {
        .name = "MIPI_2lane_RAW8_1024_600_6fps",
        .format = ESP_CAM_SENSOR_PIXFORMAT_RAW8,
        .port = ESP_CAM_SENSOR_MIPI_CSI,
        .xclk = 38400000,
        .width = 1024,
        .height = 600,
        .regs = init_reglist_MIPI_2lane_1024_600_6fps,
        .regs_size = ARRAY_SIZE(init_reglist_MIPI_2lane_1024_600_6fps),
        .fps = 6,
        .isp_info = &mira220_isp_info[0],
        .mipi_info = {
            .mipi_clk = 400000000, 
            .lane_num = 2,
            .line_sync_en = false,
        },
        .reserved = NULL,
    }
};

static esp_err_t mira220_read(esp_sccb_io_handle_t sccb_handle, uint16_t reg, uint8_t *read_buf)
{
    return esp_sccb_transmit_receive_reg_a16v8(sccb_handle, reg, read_buf);
}

static esp_err_t mira220_write(esp_sccb_io_handle_t sccb_handle, uint16_t reg, uint8_t data)
{
    return esp_sccb_transmit_reg_a16v8(sccb_handle, reg, data);
}

/* write a array of registers  */
static esp_err_t mira220_write_array(esp_sccb_io_handle_t sccb_handle, mira220_reginfo_t *regarray)
{
    int i = 0;
    esp_err_t ret = ESP_OK;
    delay_ms(100);
    while ( (true))
    {   
        if (regarray[i].reg == 0xFFFF && regarray[i].val == 0xFF) {break;}
        ret = mira220_write(sccb_handle, regarray[i].reg, regarray[i].val);
        ESP_LOGI(TAG, "MIRA220 WRITE  0x%x = 0x%x", regarray[i].reg, regarray[i].val);
        i++;
    }
    return ret;
}

static esp_err_t mira220_set_reg_bits(esp_sccb_io_handle_t sccb_handle, uint16_t reg, uint8_t offset, uint8_t length, uint8_t value)
{
    esp_err_t ret = ESP_OK;
    uint8_t reg_data = 0;
    return ESP_OK;

    ret = mira220_read(sccb_handle, reg, &reg_data);
    if (ret != ESP_OK) {
        return ret;
    }
    uint8_t mask = ((1 << length) - 1) << offset;
    value = (ret & ~mask) | ((value << offset) & mask);
    ret = mira220_write(sccb_handle, reg, value);
}

static esp_err_t mira220_set_test_pattern(esp_cam_sensor_device_t *dev, int enable)
{
    return mira220_write(dev->sccb_handle, 0x2091, enable ? 0x01 : 0x00);
    return ESP_OK;
}

static esp_err_t mira220_hw_reset(esp_cam_sensor_device_t *dev)
{
    // if (dev->reset_pin >= 0) {
    //     gpio_set_level(dev->reset_pin, 0);
    //     delay_ms(10);
    //     gpio_set_level(dev->reset_pin, 1);
    //     delay_ms(10);
    // }
    return ESP_OK;
}

static esp_err_t mira220_soft_reset(esp_cam_sensor_device_t *dev)
{
    return ESP_OK;
}

static esp_err_t mira220_get_sensor_id(esp_cam_sensor_device_t *dev, esp_cam_sensor_id_t *id)
{

    esp_err_t ret = ESP_FAIL;
    uint8_t pid_h, pid_l;

    ret = mira220_read(dev->sccb_handle, mira220_REG_SENSOR_ID_H, &pid_h);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = mira220_read(dev->sccb_handle, mira220_REG_SENSOR_ID_L, &pid_l);
    if (ret != ESP_OK) {
        return ret;
    }
    id->pid = (pid_h << 8) | pid_l;
    ESP_LOGI(TAG, "MIRA220 READ 0x%x = 0x%x", mira220_REG_SENSOR_ID_H, pid_h);
    ESP_LOGI(TAG, "MIRA220 READ 0x%x = 0x%x", mira220_REG_SENSOR_ID_L, pid_l);

    return ESP_OK;
}

static esp_err_t mira220_set_stream(esp_cam_sensor_device_t *dev, int enable)
{
    esp_err_t ret = ESP_OK;
    ret = mira220_write(dev->sccb_handle, mira220_REG_MODE , enable ? 0x10 : 0x02);
    delay_ms(10);
    ret = mira220_write(dev->sccb_handle, mira220_REG_START, enable ? 0x01 : 0x00);
    delay_ms(10);

    dev->stream_status = enable;
    ESP_LOGD(TAG, "PEDRO SET Stream=%d", enable);
    return ESP_OK;
}

static esp_err_t mira220_set_mirror(esp_cam_sensor_device_t *dev, int enable)
{
    return mira220_write(dev->sccb_handle, 0x209C, enable ? 0x01 : 0x00);
    return ESP_OK;
}

static esp_err_t mira220_set_vflip(esp_cam_sensor_device_t *dev, int enable)
{
    return mira220_write(dev->sccb_handle, 0x1095, enable ? 0x01 : 0x00);
    return ESP_OK;
}

static esp_err_t mira220_set_exp_val(esp_cam_sensor_device_t *dev, uint32_t u32_val)
{
    esp_err_t ret;
    struct mira220_cam *cam_mira220 = (struct mira220_cam *)dev->priv;
    uint32_t value_buf = u32_val;
    ESP_LOGD(TAG, "set exposure 0x%" PRIx32, value_buf);
    // u32 time_conversion = 600 / mira220_format_info->xclk;
    // exposure_length = round(time_us / 1e6 / time_conversion)

    ret = mira220_write(dev->sccb_handle, mira220_REG_EXP_L,    value_buf&&0x00FF);
    ret = mira220_write(dev->sccb_handle, mira220_REG_EXP_H,    value_buf&&0xFF00);

    if (ret == ESP_OK) {         cam_mira220->mira220_para.exposure_val = value_buf;    }
    return ESP_OK;
}

static esp_err_t mira220_set_total_gain_val(esp_cam_sensor_device_t *dev, uint32_t u32_val)
{
    esp_err_t ret = ESP_OK;
    return ret;
}

static esp_err_t mira220_query_para_desc(esp_cam_sensor_device_t *dev, esp_cam_sensor_param_desc_t *qdesc)
{
    esp_err_t ret = ESP_OK;

    switch (qdesc->id) {
    case ESP_CAM_SENSOR_EXPOSURE_VAL:
        qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_NUMBER;
        qdesc->number.minimum = 0xF;
        qdesc->number.maximum = 0x0FFF; 
        qdesc->number.step = 1;
        qdesc->default_value = dev->cur_format->isp_info->isp_v1_info.exp_def;
        break;
    // case ESP_CAM_SENSOR_GAIN:
    //     qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_ENUMERATION;
    //     qdesc->enumeration.count = s_limited_gain_index;
    //     //qdesc->enumeration.elements = mira220_total_gain_val_map;
    //     qdesc->default_value = dev->cur_format->isp_info->isp_v1_info.gain_def; // gain index
    //     break;
    // case ESP_CAM_SENSOR_GROUP_EXP_GAIN:
    //     qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_U8;
    //     qdesc->u8.size = sizeof(esp_cam_sensor_gh_exp_gain_t);
    //     break;
    case ESP_CAM_SENSOR_VFLIP:
            break;
    case ESP_CAM_SENSOR_HMIRROR:
        qdesc->type = ESP_CAM_SENSOR_PARAM_TYPE_NUMBER;
        qdesc->number.minimum = 0;
        qdesc->number.maximum = 1;
        qdesc->number.step = 1;
        qdesc->default_value = 0;
        break;
    default: 
        ESP_LOGD(TAG, "id=%"PRIx32" is not supported", qdesc->id);
        ret = ESP_ERR_INVALID_ARG;
        break;    
    }
    return ret;
}

static esp_err_t mira220_get_para_value(esp_cam_sensor_device_t *dev, uint32_t id, void *arg, size_t size)
{
    esp_err_t ret = ESP_OK;
    return ret;
    struct mira220_cam *cam_mira220 = (struct mira220_cam *)dev->priv;
    switch (id) {
    case ESP_CAM_SENSOR_EXPOSURE_VAL: {
        *(uint32_t *)arg = cam_mira220->mira220_para.exposure_val;
        break;
    }
    case ESP_CAM_SENSOR_GAIN: {
        *(uint32_t *)arg = cam_mira220->mira220_para.gain_index;
        break;
    }
    default: {
        ret = ESP_ERR_NOT_SUPPORTED;
        break;
    }
    }
    
}

static esp_err_t mira220_set_para_value(esp_cam_sensor_device_t *dev, uint32_t id, const void *arg, size_t size)
{
    esp_err_t ret = ESP_OK;

    switch (id) 
    {
    case ESP_CAM_SENSOR_EXPOSURE_VAL: {
        uint32_t u32_val = *(uint32_t *)arg;
        ret = mira220_set_exp_val(dev, u32_val);
        break;
    }
    // case ESP_CAM_SENSOR_EXPOSURE_US: {
    //     uint32_t u32_val = *(uint32_t *)arg;
    //     uint32_t ori_exp = EXPOSURE_V4L2_TO_mira220(u32_val, dev->cur_format);
    //     ret = mira220_set_exp_val(dev, ori_exp);
    //     break;
    // }
    // case ESP_CAM_SENSOR_GAIN: {
    //     uint32_t u32_val = *(uint32_t *)arg;
    //     //ret = mira220_set_total_gain_val(dev, u32_val);
    //     break;
    // }
    // case ESP_CAM_SENSOR_GROUP_EXP_GAIN: {
    //     esp_cam_sensor_gh_exp_gain_t *value = (esp_cam_sensor_gh_exp_gain_t *)arg;
    //     uint32_t ori_exp = EXPOSURE_V4L2_TO_mira220(value->exposure_us, dev->cur_format);
    //     ret = mira220_write(dev->sccb_handle, mira220_REG_GROUP_HOLD, mira220_GROUP_HOLD_START);
    //     ret |= mira220_set_exp_val(dev, ori_exp);
    //     //ret |= mira220_set_total_gain_val(dev, value->gain_index);
    //     ret |= mira220_write(dev->sccb_handle, mira220_REG_GROUP_HOLD_DELAY, mira220_GROUP_HOLD_DELAY_FRAMES);
    //     ret |= mira220_write(dev->sccb_handle, mira220_REG_GROUP_HOLD, mira220_GROUP_HOLD_END);
    //     break;
    // }
    case ESP_CAM_SENSOR_VFLIP: {
        int *value = (int *)arg;
        ret = mira220_set_vflip(dev, *value);
        break;
    }
    case ESP_CAM_SENSOR_HMIRROR: {
        int *value = (int *)arg;
        ret = mira220_set_mirror(dev, *value);
        break;
    }
    default: {
        ESP_LOGE(TAG, "set id=%" PRIx32 " is not supported", id);
        ret = ESP_ERR_INVALID_ARG;
        break;
    }
    }

    return ret;
}

static esp_err_t mira220_query_support_formats(esp_cam_sensor_device_t *dev, esp_cam_sensor_format_array_t *formats)
{
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, dev);
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, formats);

    formats->count = ARRAY_SIZE(mira220_format_info);
    formats->format_array = &mira220_format_info[0];
    return ESP_OK;
}

static esp_err_t mira220_query_support_capability(esp_cam_sensor_device_t *dev, esp_cam_sensor_capability_t *sensor_cap)
{
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, dev);
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, sensor_cap);

    sensor_cap->fmt_raw = 1;
    return 0;
}

static esp_err_t mira220_set_format(esp_cam_sensor_device_t *dev, const esp_cam_sensor_format_t *format)
{
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, dev);
    struct mira220_cam *cam_mira220 = (struct mira220_cam *)dev->priv;
    esp_err_t ret = ESP_OK;
    /* Depending on the interface type, an available configuration is automatically loaded.
    You can set the output format of the sensor without using query_format().*/
    format = &mira220_format_info[CONFIG_CAMERA_MIRA220_MIPI_IF_FORMAT_INDEX_DEFAULT];
    ret = mira220_write_array(dev->sccb_handle, (mira220_reginfo_t *)format->regs);

    // if (ret != ESP_OK) {
    //     ESP_LOGE(TAG, "Set format regs fail");
    //     return ESP_CAM_SENSOR_ERR_FAILED_SET_FORMAT;
    // }

    dev->cur_format = format;
    // // init para


    return ESP_OK;
}

static esp_err_t mira220_get_format(esp_cam_sensor_device_t *dev, esp_cam_sensor_format_t *format)
{
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, dev);
    ESP_CAM_SENSOR_NULL_POINTER_CHECK(TAG, format);
    
    esp_err_t ret = ESP_FAIL;

    if (dev->cur_format != NULL) {
        memcpy(format, dev->cur_format, sizeof(esp_cam_sensor_format_t));
        ret = ESP_OK;
    }
    return ret;
}

static esp_err_t mira220_priv_ioctl(esp_cam_sensor_device_t *dev, uint32_t cmd, void *arg)
{
    esp_err_t ret = ESP_OK;
    uint8_t regval;
    ESP_LOGE(TAG, "mira220_priv_ioctl");

    esp_cam_sensor_reg_val_t *sensor_reg;
    mira220_IO_MUX_LOCK(mux);

    switch (cmd) {
    //case ESP_CAM_SENSOR_IOC_HW_RESET:
        //ret = mira220_hw_reset(dev);
        //break;
    //case ESP_CAM_SENSOR_IOC_SW_RESET:
        //ret = mira220_soft_reset(dev);
        //break;
    //case ESP_CAM_SENSOR_IOC_S_REG:
        //sensor_reg = (esp_cam_sensor_reg_val_t *)arg;
        //ret = mira220_write(dev->sccb_handle, sensor_reg->regaddr, sensor_reg->value);
        //break;
    case ESP_CAM_SENSOR_IOC_S_STREAM:
        ret = mira220_set_stream(dev, *(int *)arg);
        ESP_LOGE(TAG, "SET STREAM");
        break;
    case ESP_CAM_SENSOR_IOC_S_TEST_PATTERN:
        ret = mira220_set_test_pattern(dev, *(int *)arg);
        break;
    //case ESP_CAM_SENSOR_IOC_G_REG:
        //sensor_reg = (esp_cam_sensor_reg_val_t *)arg;
        //ret = mira220_read(dev->sccb_handle, sensor_reg->regaddr, &regval);
        //if (ret == ESP_OK) {
        //    sensor_reg->value = regval;
        //}
        //break;
    //case ESP_CAM_SENSOR_IOC_G_CHIP_ID:
        //ret = mira220_get_sensor_id(dev, arg);
        //break;
    default:
        break;
    }

    mira220_IO_MUX_UNLOCK(mux);
    return ret;
}

static esp_err_t mira220_power_on(esp_cam_sensor_device_t *dev)
{
    esp_err_t ret = ESP_OK;

    // if (dev->xclk_pin >= 0) {
    //     //mira220_ENABLE_OUT_XCLK(dev->xclk_pin, dev->xclk_freq_hz);
    // }

    ESP_LOGE(TAG, "MIRA POWER ONNNN");
    return ret;
}

static esp_err_t mira220_power_off(esp_cam_sensor_device_t *dev)
{
    esp_err_t ret = ESP_OK;

    // if (dev->xclk_pin >= 0) {
    //     mira220_DISABLE_OUT_XCLK(dev->xclk_pin);
    // }

    return ret;
}

static esp_err_t mira220_delete(esp_cam_sensor_device_t *dev)
{
    if (dev) {
        if (dev->priv) {
            free(dev->priv);
            dev->priv = NULL;
        }
        free(dev);
        dev = NULL;
    }

    return ESP_OK;
}

static const esp_cam_sensor_ops_t mira220_ops = {
    .query_para_desc = mira220_query_para_desc,
    .get_para_value = mira220_get_para_value,
    .set_para_value = mira220_set_para_value,
    .query_support_formats = mira220_query_support_formats,
    .query_support_capability = mira220_query_support_capability,
    .set_format = mira220_set_format,
    .get_format = mira220_get_format,
    .priv_ioctl = mira220_priv_ioctl,
    .del = mira220_delete
};

esp_cam_sensor_device_t *mira220_detect(esp_cam_sensor_config_t *config)
{
    esp_cam_sensor_device_t *dev = NULL;
    struct mira220_cam *cam_mira220;


    dev = calloc(1, sizeof(esp_cam_sensor_device_t));
    if (dev == NULL) {
        return NULL;
    }

    cam_mira220 = heap_caps_calloc(1, sizeof(struct mira220_cam), MALLOC_CAP_DEFAULT);
    if (!cam_mira220) {
        free(dev);
        return NULL;
    }

    dev->name = (char *)mira220_SENSOR_NAME;
    dev->sccb_handle = config->sccb_handle;
    dev->xclk_pin = config->xclk_pin;
    dev->reset_pin = config->reset_pin;
    dev->pwdn_pin = config->pwdn_pin;
    dev->sensor_port = config->sensor_port;
    dev->ops = &mira220_ops;
    dev->priv = cam_mira220;
    dev->cur_format = &mira220_format_info[CONFIG_CAMERA_MIRA220_MIPI_IF_FORMAT_INDEX_DEFAULT];




    // Configure sensor power, clock, and SCCB port
    if (mira220_power_on(dev) != ESP_OK) {
        goto err_free_handler;
    }

    // if (mira220_get_sensor_id(dev, &dev->id) != ESP_OK) {
    //     ESP_LOGE(TAG, "Get sensor ID failed");
    //     goto err_free_handler;
    // } else if (dev->id.pid != mira220_PID) {
    //     ESP_LOGE(TAG, "Camera sensor is not mira220, PID=0x%x", dev->id.pid);
    //     goto err_free_handler;
    // }

    return dev;

err_free_handler:
    mira220_power_off(dev);
    free(dev->priv);
    free(dev);

    return NULL;
}

#if 1
ESP_CAM_SENSOR_DETECT_FN(mira220_detect, ESP_CAM_SENSOR_MIPI_CSI, mira220_SCCB_ADDR)
{
    ((esp_cam_sensor_config_t *)config)->sensor_port = ESP_CAM_SENSOR_MIPI_CSI;
    return mira220_detect(config);
}
#endif
