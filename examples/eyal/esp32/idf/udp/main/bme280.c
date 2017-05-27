/* See: https://github.com/yanbe/bme280-esp-idf-i2c/blob/master/main/main.c
 * BoschSensortec routines here: https://github.com/BoschSensortec/BME280_driver/tree/master
*/

#include "udp.h"
#include <driver/i2c.h>

#define I2C_NUM				I2C_NUM_0
#define I2C_FREQ_HZ			1000000
#define I2C_RX_BUF_DISABLE		0
#define I2C_TX_BUF_DISABLE		0

#define	BME280_I2C_ADDR			0x76	// or 0x77 with SDO to GND

#define BME280_REGISTER_CONTROL		0xF4
#define BME280_REGISTER_CONTROL_HUM	0xF2
#define BME280_REGISTER_CONFIG		0xF5
#define BME280_REGISTER_CHIPID		0xD0
#define BME280_REGISTER_VERSION		0xD1
#define BME280_REGISTER_SOFTRESET	0xE0
#define BME280_REGISTER_CAL26		0xE1
#define BME280_REGISTER_PRESS		0xF7	// 0xF7-0xF9
#define BME280_REGISTER_TEMP		0xFA	// 0xFA-0xFC
#define BME280_REGISTER_HUM		0xFD	// 0xFD-0xFE

#define BME280_REGISTER_DIG_T		0x88	// 0x88-0x8D ( 6)
#define BME280_REGISTER_DIG_P		0x8E	// 0x8E-0x9F (18)
#define BME280_REGISTER_DIG_H1		0xA1	// 0xA1      ( 1)
#define BME280_REGISTER_DIG_H2		0xE1	// 0xE1-0xE7 ( 7)

#define BME280_SLEEP_MODE		0x00
#define BME280_FORCED_MODE		0x01
#define BME280_NORMAL_MODE		0x03
#define BME280_SOFT_RESET_CODE		0xB6

#define ACK_CHECK_EN			0x1
#define ACK_VAL				0
#define NAK_VAL				1

#define BME280_S32_t	int32_t
#define BME280_U32_t	uint32_t
#define BME280_S64_t	int64_t

#define BME280_SAMPLING_DELAY		113	// maximum measurement time in ms for maximum
	// oversampling for all measures = 1.25 + 2.3*16 + 2.3*16 + 0.575 + 2.3*16 + 0.575 ms

static struct {
	uint16_t  dig_T1;
	int16_t   dig_T2;
	int16_t   dig_T3;
	uint16_t  dig_P1;
	int16_t   dig_P2;
	int16_t   dig_P3;
	int16_t   dig_P4;
	int16_t   dig_P5;
	int16_t   dig_P6;
	int16_t   dig_P7;
	int16_t   dig_P8;
	int16_t   dig_P9;
	uint8_t   dig_H1;
	int16_t   dig_H2;
	uint8_t   dig_H3;
	int16_t   dig_H4;
	int16_t   dig_H5;
	int8_t    dig_H6;
} bme280_data;

static BME280_S32_t bme280_t_fine;	// not yet used

static i2c_port_t i2c_num = I2C_NUM;
static uint8_t bme280_isbme = 0;	// 1 if BME280, 0 if BMP280
static uint8_t bme280_mode = 0;		// stores oversampling settings
static uint8_t bme280_ossh = 0;		// stores humidity oversampling settings
static int have_bme280 = 0;

static esp_err_t i2c_master_init(uint8_t sda, uint8_t scl)
{
    int i2c_master_port = I2C_NUM;
    i2c_config_t conf;
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = sda;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_io_num = scl;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = I2C_FREQ_HZ;
    Dbg (i2c_param_config(i2c_master_port, &conf));
    Dbg (i2c_driver_install(i2c_master_port, conf.mode,
                       I2C_RX_BUF_DISABLE,
                       I2C_TX_BUF_DISABLE, 0));

    return ESP_OK;
}

static esp_err_t i2c_master_address (
	i2c_cmd_handle_t cmd, uint8_t id, int dir)
{
	Dbg (i2c_master_write_byte(cmd, (id << 1) | dir, ACK_CHECK_EN));

	return ESP_OK;
}

static esp_err_t i2c_read_bytes(
	uint8_t addr, uint8_t reg, uint8_t *buf, size_t buflen)
{
        esp_err_t ret;
	i2c_cmd_handle_t cmd;

	if (buflen < 1)
		return ESP_OK;

// send address read request
	cmd = i2c_cmd_link_create();
	Dbg (i2c_master_start(cmd));
	Dbg (i2c_master_address (cmd, addr, I2C_MASTER_WRITE));
	Dbg (i2c_master_write_byte(cmd, reg, ACK_CHECK_EN));
///	Dbg (i2c_master_stop(cmd));
///	ret = i2c_master_cmd_begin(i2c_num, cmd, 10 / portTICK_RATE_MS);
///	i2c_cmd_link_delete(cmd);
///	Dbg (((void)"i2c_master_cmd_begin", ret));

// receive data
///	cmd = i2c_cmd_link_create();
	Dbg (i2c_master_start(cmd));
	Dbg (i2c_master_address (cmd, addr, I2C_MASTER_READ));
	if (buflen > 1)
		Dbg (i2c_master_read(cmd, buf, buflen-1, ACK_VAL));
	Dbg (i2c_master_read_byte(cmd, buf+buflen-1, NAK_VAL));
	Dbg (i2c_master_stop(cmd));
	ret = i2c_master_cmd_begin(i2c_num, cmd, 10 / portTICK_RATE_MS);
	i2c_cmd_link_delete(cmd);
	Dbg (((void)"i2c_master_cmd_begin", ret));

	return ESP_OK;
}

static esp_err_t i2c_write_byte(
	uint8_t addr, uint8_t reg, uint8_t val)
{
        esp_err_t ret;
	i2c_cmd_handle_t cmd;

	cmd = i2c_cmd_link_create();
        Dbg (i2c_master_start(cmd));
        Dbg (i2c_master_address(cmd, addr, I2C_MASTER_WRITE));
        Dbg (i2c_master_write_byte(cmd, reg, ACK_CHECK_EN));
        Dbg (i2c_master_write_byte(cmd, val, ACK_CHECK_EN));
        Dbg (i2c_master_stop(cmd));
	ret = i2c_master_cmd_begin(i2c_num, cmd, 10 / portTICK_RATE_MS);
	i2c_cmd_link_delete(cmd);
	if (ret != ESP_OK) {Dbg (((void)"i2c_master_cmd_begin", ret));}

	return ESP_OK;
}

static int32_t bme280_compensate_T (int32_t adc_T)
{
	BME280_S32_t var1, var2, T;

	var1  = ((((adc_T>>3) - ((BME280_S32_t)bme280_data.dig_T1<<1))) * ((BME280_S32_t)bme280_data.dig_T2)) >> 11;
	var2  = (((((adc_T>>4) - ((BME280_S32_t)bme280_data.dig_T1)) * ((adc_T>>4) - ((BME280_S32_t)bme280_data.dig_T1))) >> 12) * ((BME280_S32_t)bme280_data.dig_T3)) >> 14;
	bme280_t_fine = var1 + var2;
	T  = (bme280_t_fine * 5 + 128) >> 8;

	return T;
}

static esp_err_t i2c_bme280_startreadout (int read_delay_ms)
{
	if (read_delay_ms <= 0)
		read_delay_ms = BME280_SAMPLING_DELAY;

	Dbg (i2c_write_byte (BME280_I2C_ADDR, BME280_REGISTER_CONTROL_HUM, bme280_ossh));
	Dbg (i2c_write_byte (BME280_I2C_ADDR, BME280_REGISTER_CONTROL, (bme280_mode & 0xFC) | BME280_FORCED_MODE));

	if (read_delay_ms > 10)
		delay (read_delay_ms);

	return ESP_OK;
}

static esp_err_t i2c_bme280_read (uint8_t *buf, size_t buflen)
{
	Dbg (i2c_read_bytes (BME280_I2C_ADDR, BME280_REGISTER_PRESS, buf, buflen));

	return ESP_OK;
}

float bme280_temp (void)
{
	esp_err_t ret;
	uint8_t buf[8];	// registers are P[3], T[3], H[2]
	int32_t adc_T;	//, adc_P, adc_H;

	if (!have_bme280)
		adc_T = 9999;
	else {
//		Dbg (i2c_bme280_startreadout (200));
		memset (buf, 0, sizeof (buf));
		ret = i2c_bme280_read (buf, sizeof(buf));
		if (ret != ESP_OK)
			adc_T = 9998;
		else {
//			adc_P = (int32_t)((buf[0] << 12) | (buf[1] << 4) | (buf[2] >> 4));
			adc_T = (int32_t)((buf[3] << 12) | (buf[4] << 4) | (buf[5] >> 4));
//			adc_H = (int32_t)((buf[6] <<  8) | buf[7]);

			adc_T = bme280_compensate_T(adc_T);
//			qfe   = bme280_compensate_P(adc_P);
//			adc_H = bme280_compensate_H(adc_H);
//			qnh   = bme280_qfe2qnh(qfe, h);
		}
		Dbg (i2c_bme280_startreadout (1));
	}
	Log("t=%.2f %02x%02x%02x %02x%02x%02x %02x%02x", adc_T/100.,
		buf[0], buf[1], buf[2],
		buf[3], buf[4], buf[5],
		buf[6], buf[7]);

	return adc_T/100.;
}

static esp_err_t i2c_bme280_setup(
	uint8_t p1, uint8_t p2, uint8_t p3, uint8_t p4, uint8_t p5, uint8_t p6, uint8_t full_init)
{
	uint8_t config;
	uint8_t	buf[18], *reg;

	uint8_t const bit3 = 0b111;
	uint8_t const bit2 = 0b11;

	bme280_mode =
		(p4&bit2) |		// 4-th parameter: power mode
		(p2&bit3 << 2) |	// 2-nd parameter: pressure oversampling
		(p1&bit3 << 5);		// 1-st parameter: temperature oversampling
		
	bme280_ossh = p3&bit3;		// 3-rd parameter: humidity oversampling
	
	config = 
		(p5&bit3 << 5) |	// 5-th parameter: inactive duration in normal mode
		(p6&bit3 << 2);		// 6-th parameter: IIR filter
	
	Dbg (i2c_read_bytes (BME280_I2C_ADDR, BME280_REGISTER_CHIPID, buf, 1));
	uint8_t chipid = (uint8_t)buf[0];
	bme280_isbme = (chipid == 0x60);
	Log("CHIPID=0x%2x", chipid);
	
#define r16uLE_buf(reg)	(uint16_t)((reg[1] << 8) | reg[0])
#define r16sLE_buf(reg)	(int16_t)(r16uLE_buf(reg))

	Dbg (i2c_read_bytes (BME280_I2C_ADDR, BME280_REGISTER_DIG_T, buf, 6));
	reg = buf;
	bme280_data.dig_T1 = r16uLE_buf(reg); reg+=2;
	bme280_data.dig_T2 = r16sLE_buf(reg); reg+=2;
	bme280_data.dig_T3 = r16sLE_buf(reg);

	Dbg (i2c_read_bytes (BME280_I2C_ADDR, BME280_REGISTER_DIG_P, buf, 18));
	reg = buf;
	bme280_data.dig_P1 = r16uLE_buf(reg); reg+=2;
	bme280_data.dig_P2 = r16sLE_buf(reg); reg+=2;
	bme280_data.dig_P3 = r16sLE_buf(reg); reg+=2;
	bme280_data.dig_P4 = r16sLE_buf(reg); reg+=2;
	bme280_data.dig_P5 = r16sLE_buf(reg); reg+=2;
	bme280_data.dig_P6 = r16sLE_buf(reg); reg+=2;
	bme280_data.dig_P7 = r16sLE_buf(reg); reg+=2;
	bme280_data.dig_P8 = r16sLE_buf(reg); reg+=2;
	bme280_data.dig_P9 = r16sLE_buf(reg);
	
	if (full_init)
		Dbg (i2c_write_byte (BME280_I2C_ADDR, BME280_REGISTER_CONFIG, config));

	if (bme280_isbme) {
		Dbg (i2c_read_bytes (BME280_I2C_ADDR, BME280_REGISTER_DIG_H1, buf, 1));
		bme280_data.dig_H1 = buf[0];

		Dbg (i2c_read_bytes (BME280_I2C_ADDR, BME280_REGISTER_DIG_H2, buf, 7));
		reg = buf;
		bme280_data.dig_H2 = r16sLE_buf(reg); reg+=2;
		bme280_data.dig_H3 = reg[0]; reg++;
		bme280_data.dig_H4 = (int16_t)reg[0] << 4 | (reg[1] & 0x0F); reg+=1;	// H4[11:4 3:0] = 0xE4[7:0] 0xE5[3:0] 12-bit signed
		bme280_data.dig_H5 = (int16_t)reg[1] << 4 | (reg[0]   >> 4); reg+=2;	// H5[11:4 3:0] = 0xE6[7:0] 0xE5[7:4] 12-bit signed
		bme280_data.dig_H6 = (int8_t)reg[0];

		if (full_init)
			Dbg (i2c_write_byte (BME280_I2C_ADDR, BME280_REGISTER_CONTROL_HUM, bme280_ossh));
	}
#undef r16uLE_buf
#undef r16sLE_buf
	if (full_init)
		Dbg (i2c_write_byte (BME280_I2C_ADDR, BME280_REGISTER_CONTROL, bme280_mode));
	
	return ESP_OK;
}

esp_err_t bme280_init (uint8_t sda, uint8_t scl, int full)
{
    have_bme280 = 0;

    Dbg (i2c_master_init(sda, scl));

    Dbg (i2c_bme280_setup (
	1,			// oversampling x1 (read once)
	1,			// oversampling x1 (read once)
	1,			// oversampling x1 (read once)
	BME280_SLEEP_MODE,	// power mode
	0,			// filter off
	0,			// inactive_duration (not used)
	1+0*full));		// init the chip too (we do on cold start)

    have_bme280 = 1;
    return ESP_OK;
}
