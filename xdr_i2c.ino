/*
 *  XDR-I2C 2013-08-06
 *  Copyright (C) 2012-2013  Konrad Kosmatka
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation; either version 2
 *  of the License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#define IR 0
/* If you have an IR diode, change this to 1 */

#include <Arduino.h>
#include <I2cMaster.h>
#include <avr/pgmspace.h>
#include "filters.h"
#include "xdr_f1hd.h"

#define RDS_PIN    2
#define IR_PIN     3
#define RESET_PIN  4
#define SDA_PIN   A4
#define SCL_PIN   A5

#define PI_BUFFER_SIZE 64
#define SERIAL_BUFFER_SIZE 16
#define INIT 0

#define MODE_FM 0
#define MODE_AM 1

char buff[SERIAL_BUFFER_SIZE];
uint8_t buff_pos = 0;

int8_t current_filter = -1;
uint8_t mode;

uint8_t CONTROL = 0x00;
uint16_t PLL;
uint8_t DAA = 0x00;
uint8_t AGC = 0xC8;
uint8_t BAND;

uint16_t pi_buffer[PI_BUFFER_SIZE];
uint8_t rds_buffer[4], rds_status_buffer, pi_pos = 0;
bool pi_checked = false;

uint32_t timer = 0, rds_timer = 0;

bool spectrum_flag = false;
uint32_t spectrum_start = 0, spectrum_end = 0;

TwiMaster i2c(false);

uint8_t dsp_query(uint8_t, uint8_t, uint8_t);
void dsp_write_data(const uint8_t*);
void dsp_write_coeff(uint8_t, uint8_t);
void dsp_read_rds();
void dsp_set_filter(int8_t);
void dsp_set_deemphasis(uint8_t);
void tune(boolean);
void tune_freq(uint32_t, boolean);
void spectrum(uint16_t);
void serial_hex(uint8_t);
void sendcode(uint32_t code);
void carrier(int time);
void start();
void one();
void zero();
void align(uint32_t);

#define ADDR1(a) ((a & 0xFF0000) >> 16)
#define ADDR2(a) ((a & 0xFF00) >> 8)
#define ADDR3(a) (a & 0xFF)

void setup(void)
{
    pinMode(RDS_PIN, INPUT);
    pinMode(SDA_PIN, INPUT);
    pinMode(SCL_PIN, INPUT);
    pinMode(RESET_PIN, OUTPUT);
    digitalWrite(RESET_PIN, LOW);
    pinMode(IR_PIN, OUTPUT);
    Serial.begin(115200);

    while(true)
    {
        if(Serial.available() > 0)
        {
            if(Serial.read() == 'x')
            {
                while(!Serial.available());
                if(Serial.read() == '\n')
                    break;
            }
        }
    }

#if IR
    for(uint8_t i=0; i<10; i++)
    {
        sendcode(0xA8BC8);
        delayMicroseconds(10000);
    }
    delay(5500);
#endif

    digitalWrite(RESET_PIN, HIGH);
    pinMode(SDA_PIN, OUTPUT);
    pinMode(SCL_PIN, OUTPUT);
    digitalWrite(SDA_PIN, HIGH);
    digitalWrite(SCL_PIN, HIGH);
    delay(100);

#if INIT
    dsp_write_data(DSP_INIT);
#endif

    mode_FM();
    tune_freq(87500, false);
    dsp_set_deemphasis(0); // 50us de-emphasis as default

    while(Serial.available() > 0)
        Serial.read(); // clear the serial buffer
    Serial.print("\nOK\n");
}

void loop()
{
    if(!digitalRead(RDS_PIN))
        dsp_read_rds();

    // check signal level and 19kHz pilot indicator ~15 times per second
    if((millis()-timer) >= 67)
    {
        uint32_t buffer;
        i2c.start(DSP_I2C | I2C_WRITE);
        if(mode == MODE_FM)
        {
            i2c.write(ADDR1(DSP_FM_LEVEL));
            i2c.write(ADDR2(DSP_FM_LEVEL));
            i2c.write(ADDR3(DSP_FM_LEVEL));
        }
        else
        {
            i2c.write(ADDR1(DSP_AM_LEVEL));
            i2c.write(ADDR2(DSP_AM_LEVEL));
            i2c.write(ADDR3(DSP_AM_LEVEL));
        }
        i2c.restart(DSP_I2C | I2C_READ);
        buffer = ((uint32_t)i2c.read(false) << 16);
        buffer |= ((uint16_t)i2c.read(false) << 8);
        buffer |= i2c.read(true);
        i2c.stop();

        Serial.print('S');
        if(mode == MODE_FM && dsp_query(ADDR1(DSP_ST_19kHz), ADDR2(DSP_ST_19kHz), ADDR3(DSP_ST_19kHz)))
        {
            Serial.print('s'); // 19kHz pilot
        }
        else
        {
            Serial.print('m');
        }
        Serial.print(buffer, DEC);
        Serial.print('\n');
        timer = millis();
    }

    if(Serial.available() > 0)
    {
        buff[buff_pos] = Serial.read();
        if(buff[buff_pos] != '\n' && buff_pos != SERIAL_BUFFER_SIZE-1)
            buff_pos++;
        else
        {
            buff[buff_pos] = 0x00;
            buff_pos = 0;
            switch (buff[0])
            {
            case 'x':
                Serial.print("OK\n");
                break;

            case 'T': // frequency change
                tune_freq(atol(buff+1), true);
                break;

            case 'A': // RF AGC threshold
                switch (atoi(buff+1))
                {
                case 0:
                    AGC &= B11110011; // highest
                    break;
                case 1:
                    AGC &= B11110111; // high
                    AGC |= B00000100;
                    break;
                case 2:
                    AGC &= B11111011; // medium
                    AGC |= B00001000;
                    break;
                case 3:
                    AGC |= B00001100; // low
                    break;
                default:
                    return;
                }
                tune(false);
                break;

            case 'V': // set 1st antenna circuit tuning voltage
                DAA = atoi(buff+1) & 0x7F;
                tune(false);
                break;

            case 'F': // change FIR filters
                dsp_set_filter(atoi(buff+1));
                break;

            case 'D': // change the de-emphasis
                dsp_set_deemphasis(atoi(buff+1));
                break;

            case 'M': // change the mode (added by F4CMB)
                switch(atoi(buff+1))
                {
                case MODE_FM:
                    mode_FM();
                    break;
                case MODE_AM:
                    mode_AM();
                    break;
                }
                tune(false);
                break;

            case 'G':
                if(buff[1] == '1')
                {
                    CONTROL |= B10000000; // FM RF +6dB gain
                }
                else
                {
                    CONTROL &= B01111111; // FM RF standard gain
                }

                if(buff[2] == '1')
                {
                    CONTROL |= B00010000; // IF +6dB gain
                }
                else
                {
                    CONTROL &= B11101111; // IF standard gain
                }
                tune(false);
                break;

            case 'S':
                if(buff[1] == 'a')
                    spectrum_start = atol(buff+2);
                else if(buff[1] == 'b')
                    spectrum_end = atol(buff+2);
                else if(spectrum_start > 0 && spectrum_end > 0)
                    spectrum(atoi(buff+1));
                break;

            case 'X': // shutdown
                TWCR = 0; // release SDA and SCL lines used by hardware I2C
                digitalWrite(RESET_PIN, LOW);
                Serial.print("X\n");
                delay(10);
                asm("jmp 0");
                break;
            }
        }
    }
}

uint8_t dsp_query(uint8_t addr1, uint8_t addr2, uint8_t addr3)
{
    i2c.start(DSP_I2C | I2C_WRITE);
    i2c.write(addr1);
    i2c.write(addr2);
    i2c.write(addr3);
    i2c.restart(DSP_I2C | I2C_READ);
    uint8_t buffer = i2c.read(true);
    i2c.stop();
    return buffer;
}

void dsp_write_data(const uint8_t* data)
{
    uint16_t i = 1;
    uint8_t len = pgm_read_byte_near(data), pos;
    while(len != 0x00)
    {
        i2c.start(DSP_I2C | I2C_WRITE);
        for(pos=0; pos<len; pos++)
            i2c.write(pgm_read_byte_near(data+i+pos));
        i2c.stop();
        i += pos;
        len = pgm_read_byte_near(data+(i++));
    }
}

void dsp_write_coeff(uint8_t bank, uint8_t filter)
{
    uint8_t i = 0;
    uint16_t address = 0x0C00 + 32 * bank;
    while(i<64)
    {
        i2c.start(DSP_I2C | I2C_WRITE);
        i2c.write(0x01);
        i2c.write(address >> 8);
        i2c.write(address & 0xFF);
        i2c.write(pgm_read_byte_near(filters[filter]+(i++)));
        i2c.write(pgm_read_byte_near(filters[filter]+(i++)));
        i2c.write(0x00);
        i2c.stop();
        address++;
    }
}

void dsp_set_filter(int8_t f)
{
    current_filter = f;
    if(f >= 0) // fixed filter bandwidth
    {
        if(mode == MODE_AM)
        {
            // workaround for AM (?)
            for(uint8_t i=0; i<16; i++)
                dsp_write_coeff(i, f);
            return;
        }

        // write the FIR filter coefficients into $15 filter bank
        dsp_write_coeff(15, f);

        i2c.start(DSP_I2C | I2C_WRITE);
        i2c.write(ADDR1(TDSP1_X_CIBW_1_FirCtlFix));
        i2c.write(ADDR2(TDSP1_X_CIBW_1_FirCtlFix));
        i2c.write(ADDR3(TDSP1_X_CIBW_1_FirCtlFix));
        i2c.write(0x00);
        i2c.write(0x00);
        i2c.write(0x0F); // $15 filter
        i2c.stop();

        i2c.start(DSP_I2C | I2C_WRITE);
        i2c.write(ADDR1(TDSP1_X_CIBW_4_FirCtlFix));
        i2c.write(ADDR2(TDSP1_X_CIBW_4_FirCtlFix));
        i2c.write(ADDR3(TDSP1_X_CIBW_4_FirCtlFix));
        i2c.write(0x00);
        i2c.write(0x00);
        i2c.write(0x0F); // $15 filter
        i2c.stop();

        i2c.start(DSP_I2C | I2C_WRITE);
        i2c.write(ADDR1(TDSP1_X_CIBW_1_pFirCtl));
        i2c.write(ADDR2(TDSP1_X_CIBW_1_pFirCtl));
        i2c.write(ADDR3(TDSP1_X_CIBW_1_pFirCtl));
        i2c.write(0x00); // relative address!
        i2c.write(ADDR2(TDSP1_X_CIBW_1_FirCtlFix));
        i2c.write(ADDR3(TDSP1_X_CIBW_1_FirCtlFix));
        i2c.stop();

        i2c.start(DSP_I2C | I2C_WRITE);
        i2c.write(ADDR1(TDSP1_X_CIBW_4_pFirCtl));
        i2c.write(ADDR2(TDSP1_X_CIBW_4_pFirCtl));
        i2c.write(ADDR3(TDSP1_X_CIBW_4_pFirCtl));
        i2c.write(0x00); // relative address!
        i2c.write(ADDR2(TDSP1_X_CIBW_4_FirCtlFix));
        i2c.write(ADDR3(TDSP1_X_CIBW_4_FirCtlFix));
        i2c.stop();

    }
    else if(mode == MODE_FM) // adaptive filter bandwidth
    {
        for(uint8_t i=0; i<16; i++)
            dsp_write_coeff(i, adaptive_filters_set[i]);

        i2c.start(DSP_I2C | I2C_WRITE);
        i2c.write(ADDR1(TDSP1_X_CIBW_1_pFirCtl));
        i2c.write(ADDR2(TDSP1_X_CIBW_1_pFirCtl));
        i2c.write(ADDR3(TDSP1_X_CIBW_1_pFirCtl));
        i2c.write(0x00); // relative address!
        i2c.write(ADDR2(TDSP1_X_CIBW_1_FirCtl));
        i2c.write(ADDR3(TDSP1_X_CIBW_1_FirCtl));
        i2c.stop();

        i2c.start(DSP_I2C | I2C_WRITE);
        i2c.write(ADDR1(TDSP1_X_CIBW_4_pFirCtl));
        i2c.write(ADDR2(TDSP1_X_CIBW_4_pFirCtl));
        i2c.write(ADDR3(TDSP1_X_CIBW_4_pFirCtl));
        i2c.write(0x00); // relative address!
        i2c.write(ADDR2(TDSP1_X_CIBW_4_FirCtl));
        i2c.write(ADDR3(TDSP1_X_CIBW_4_FirCtl));
        i2c.stop();
    }
}

void dsp_read_rds()
{
    uint8_t buffer[2], status, current_pi_count = 0;
    i2c.start(DSP_I2C | I2C_WRITE);
    i2c.write(0x00);
    i2c.write(0x00);
    i2c.write(0x30);
    i2c.restart(DSP_I2C | I2C_READ);
    i2c.read(false);
    status = i2c.read(true);
    i2c.stop();

    i2c.start(DSP_I2C | I2C_WRITE);
    i2c.write(0x00);
    i2c.write(0x00);
    i2c.write(0x31);
    i2c.restart(DSP_I2C | I2C_READ);
    buffer[0] = i2c.read(false);
    buffer[1] = i2c.read(true);
    i2c.stop();
    switch(status & B11111100)
    {
    case 0x00: // fast PI mode block
    case 0x80: // block A
    case 0x90: // block C'
        pi_buffer[pi_pos] = ((buffer[0] << 8) | buffer[1]);
        for(uint8_t i=0; i<PI_BUFFER_SIZE; i++)
            if(pi_buffer[i]==pi_buffer[pi_pos])
                current_pi_count++;

        if(current_pi_count == 2 && !pi_checked)
        {
            Serial.print('P');
            serial_hex(pi_buffer[pi_pos] >> 8);
            serial_hex(pi_buffer[pi_pos] & 0xFF);
            Serial.print("?\n");
        }
        else if(current_pi_count > 2)
        {
            Serial.print('P');
            serial_hex(pi_buffer[pi_pos] >> 8);
            serial_hex(pi_buffer[pi_pos] & 0xFF);
            Serial.print('\n');
            pi_checked = true;
        }
        pi_pos = (pi_pos+1)%PI_BUFFER_SIZE;
        break;
    case 0x84: // block B
        // we will wait for block C & D before sending anything to the serial
        rds_buffer[0] = buffer[0];
        rds_buffer[1] = buffer[1];
        rds_status_buffer = status&B11;
        rds_status_buffer |= B111100;
        rds_timer = millis();
        break;
    case 0x88: // block C
        rds_buffer[2] = buffer[0];
        rds_buffer[3] = buffer[1];
        rds_status_buffer &= B0011;
        rds_status_buffer |= (status&B11) << 2;
        break;
    case 0x8C: // block D
        // is this block related to the block B from buffer?
        if((millis()-rds_timer) < 50)
        {
            rds_status_buffer &= B001111;
            rds_status_buffer |= (status&B11) << 4;
            Serial.print('R');
            serial_hex(rds_buffer[0]);
            serial_hex(rds_buffer[1]);
            serial_hex(rds_buffer[2]);
            serial_hex(rds_buffer[3]);
            serial_hex(buffer[0]);
            serial_hex(buffer[1]);
            serial_hex(rds_status_buffer);
            Serial.print('\n');
        }
        break;
    }
}

void dsp_set_deemphasis(uint8_t d)
{
    if(d >= 3)
    {
        return;
    }
    
    i2c.start(DSP_I2C | I2C_WRITE);
    i2c.write(ADDR1(DSP_DEEMPHASIS));
    i2c.write(ADDR2(DSP_DEEMPHASIS));
    i2c.write(ADDR3(DSP_DEEMPHASIS));
    switch (d)
    {
    case 0: // 50us
        i2c.write(0x02);
        i2c.write(0xC0);
        i2c.write(0x04);
        i2c.write(0xE4);
        i2c.write(0x00);
        i2c.write(0x85);
        break;
    case 1: // 75us
        i2c.write(0x01);
        i2c.write(0xF6);
        i2c.write(0x05);
        i2c.write(0xC3);
        i2c.write(0x00);
        i2c.write(0x85);
        break;
    case 2: // off
        i2c.write(0x07);
        i2c.write(0xFF);
        i2c.write(0x00);
        i2c.write(0x00);
        i2c.write(0x00);
        i2c.write(0x00);
        break;
    }
    i2c.stop();  
}

void tune(boolean reset_rds_sync)
{
    i2c.start(DSP_I2C | I2C_WRITE);
    i2c.write(0x00);
    i2c.write(0xFF);
    i2c.write(0xFF);
    i2c.restart(IF_I2C | I2C_WRITE);
    i2c.write(0x80);
    i2c.write(CONTROL);
    i2c.write(PLL>>8);
    i2c.write(PLL&0xFF);
    i2c.write(DAA);
    i2c.write(AGC);
    i2c.write(BAND);
    i2c.stop();

    if(reset_rds_sync && !spectrum_flag)
    {
        i2c.start(DSP_I2C | I2C_WRITE);
        i2c.write(0x00);
        i2c.write(0x00);
        i2c.write(0x35);
        i2c.write(0x00);
        i2c.write(0x60); // fast pi mode
        i2c.stop();
        pi_checked = false;
    }
    delay(4);
}

void tune_freq(uint32_t freq, boolean serial_print) // ***Modified by F4CMB***
{
    if ((freq>=55000) && (freq<=137000)) // FM BAND (extended)
    {
        if(freq % 50 || freq>108000 || spectrum_flag)
        {
            PLL = ((freq+10700)*2)/10;
            BAND = B00110001; // 5kHz step, fref=10kHz
        }
        else
        {
            // we use 50kHz step if possible, because
            // in other cases the audio output isn't clear
            PLL = ((freq+10700)*2)/100;
            BAND = B00100001; // 50kHz step, fref=100kHz
        }
    }
    else if ((freq>=100) && (freq<=1900)) // LW & MW BAND
    {
        BAND = B11101101;
        PLL = ((freq+10700)*20)/20;
    }
    else if ((freq>=1901) && (freq<=5899)) // AM SW 120m to 60m
    {
        BAND = B11010001;
        PLL = ((freq+10700)*16)/10;
    }
    else if ((freq>=5900) && (freq<=13870)) // AM SW 49m to 22m
    {
        BAND = B10110001;
        PLL = ((freq+10700)*10)/10;
    }
    else if ((freq>=13871) && (freq<=19020)) // AM SW 25m to 15m
    {
        BAND = B10010001;
        PLL = ((freq+10700)*8)/10;
    }
    else if ((freq>=19021) && (freq<=30000)) // AM SW 16m to 11m
    {
        BAND = B01110001;
        PLL = ((freq+10700)*6)/10;
    }
    else
        return;

    align(freq);
    tune(true);

    if(serial_print)
    {
        Serial.print('V');
        Serial.print(DAA&0x7F, DEC);
        Serial.print("\nT");
        Serial.print(get_current_freq(), DEC);
        Serial.print('\n');
    }
}

void mode_FM()
{
    mode = MODE_FM;
    dsp_write_data(DSP_FM);
    dsp_set_filter(-1);
}

void mode_AM()
{
    mode = MODE_AM;
    dsp_write_data(DSP_AM);
}

void spectrum(uint16_t step)
{
    uint8_t _CONTROL = CONTROL;
    uint16_t _PLL = PLL;
    uint8_t _DAA = DAA;
    uint8_t _AGC = AGC;
    uint8_t _BAND = BAND; // save current settings
    uint32_t freq = spectrum_start;

    spectrum_flag = true;
    tune_freq(freq, false);
    Serial.print('U');
    while(freq<=spectrum_end)
    {
        tune_freq(freq, false);
        Serial.print(get_current_freq(), DEC);
        Serial.print('=');
        if(mode == MODE_FM)
        {
            Serial.print(dsp_query(ADDR1(DSP_FM_LEVEL), ADDR2(DSP_FM_LEVEL), ADDR3(DSP_FM_LEVEL)), DEC);
        }
        else
        {
            Serial.print(dsp_query(ADDR1(DSP_AM_LEVEL), ADDR2(DSP_AM_LEVEL), ADDR3(DSP_AM_LEVEL)), DEC);
        }
        Serial.print(',');
        freq += step;
    }
    Serial.print('\n');
    spectrum_flag = false;

    // restore previous settings
    CONTROL = _CONTROL;
    PLL = _PLL;
    DAA = _DAA;
    AGC = _AGC;
    BAND = _BAND;
    tune(true);
}

void serial_hex(uint8_t val)
{
    Serial.print(val >> 4 & 0xF, HEX);
    Serial.print(val & 0xF, HEX);
}

uint32_t get_current_freq()
{
    if(BAND == B00100001)
        return 100*(uint32_t)PLL/2-10700;
    else if(BAND == B00110001)
        return 10*(uint32_t)PLL/2-10700;
    else if(BAND == B11101101)
        return 20*(uint32_t)PLL/20-10700;
    else if(BAND == B11010001)
        return 10*(uint32_t)PLL/16-10700;
    else if(BAND == B10110001)
        return 10*(uint32_t)PLL/10-10700;
    else if(BAND == B10010001)
        return 10*(uint32_t)PLL/8-10700;
    else if(BAND == B01110001)
        return 10*(uint32_t)PLL/6-10700;
    else
        return 0;
}

/* IR support by F4CMB */
void sendcode(uint32_t code)
{
    cli();
    start();
    for (int i = 19; i >=0; i--)
    {
        if (code>>i &1 == 1) one();
        else zero();
    }
    delayMicroseconds (15000);
    start();
    for (int i = 19; i >=0; i--)
    {
        if (code>>i &1 == 1) one();
        else zero();
    }
    sei();
}

void carrier(int time)
{
    for (int i=0; i<(time/30); i++)
    {
        digitalWrite(IR_PIN, HIGH); // approx 40 KHz oscillator
        delayMicroseconds(12);
        digitalWrite(IR_PIN, LOW);
        delayMicroseconds(12);
    }
}

void start()
{
    carrier(2000);
    delayMicroseconds(600);
}

void one()
{
    carrier(1200);
    delayMicroseconds(600);
}

void zero()
{
    carrier(600);
    delayMicroseconds(600);
}

void align(uint32_t freq)
{
    // alignment of the antenna circuit
    // these values are individual for each tuner!
    if(freq>=107300)
        DAA = 52;
    else if(freq>=104500)
        DAA = 53;
    else if(freq>=100400)
        DAA = 54;
    else if(freq>=97500)
        DAA = 55;
    else if(freq>=95600)
        DAA = 56;
    else if(freq>=92900)
        DAA = 57;
    else if(freq>=90500)
        DAA = 58;
    else if(freq>=87800)
        DAA = 59;
    else if(freq>=87000)
        DAA = 60;
    else if(freq>=85000)
        DAA = 61;
    else if(freq>=84000)
        DAA = 62;
    else if(freq>=83000)
        DAA = 63;
    else if(freq>=78000)
        DAA = 64;
    else if(freq>=76000)
        DAA = 65;
    else if(freq>=74000)
        DAA = 66;
    else if(freq>=73000)
        DAA = 67;
    else if(freq>=70000)
        DAA = 69;
    else
        DAA = 70;
}

