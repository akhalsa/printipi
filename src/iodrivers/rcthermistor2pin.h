/* The MIT License (MIT)
 *
 * Copyright (c) 2014 Colin Wallace
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
 

#ifndef DRIVERS_RCTHERMISTOR_H
#define DRIVERS_RCTHERMISTOR_H

#include <cmath>
#include <utility> //for std::move
#include "platforms/auto/chronoclock.h" //for EventClockT
#include "iodrivers/iopin.h"
#include "common/mathutil.h" //for CtoK, etc
#include "common/logging.h"
#include "iodriver.h"

namespace iodrv {


/*
 * This class provides code to approximate a temperature via first determining the resistance of a thermistor 
 *   (resistor that varies its resistance according to temperature) via only a digital IO pin, fixed resistor and capacitor.
 * The raspberry pi doesn't have any ADC pins, so we must use the method outlined below.
 * The implementation is designed to be compatible with ramps-fd.
 *
 *                Vcc
 *                 \
 *                 / Rup
 *                 \
 *         Rseries /        Rchrg
 *      o---/\/\/\-+-----o-/\/\/\-o
 *      \         _|_             |
 *      / therm   ___ C           |
 *      \          |              |
 *   THERMPIN     GND          CHRG/MEAS
 *
 *  On ramps-fd Rup = 4.7k, Rseries = 22 ohm.
 *  Rchrg can be anything > ~200 ohms - make sure not to draw more than 16 mA from a pin.
 *  If drawing a significant amount of current, make sure to configure the Pi's pin drive strength first.
 *  Recommended Rchrg value is 1k ohm. The ratio Rchrg / Rup should be significantly less than 0.5 to ensure that the pin state actually changes
 *  Also, Rseries should be such that the thermistor resistance + Rseries is always such that < 16 mA is drawn.
 *
 *  Note: in the case that Rchrg = 0, therm = infinite, discharge time = 0.69*RC = 32.8 mS with Rup=4k7, C=10.1e-6, Vtoggle=1.65
 *    so that's the absolute longest discharge time.
 *
 *  TODO: given this thermistor circuit, it is possible to achieve some amount of auto-calibration upon boot.
 *    Pull CHRG low, disactivate THERMPIN, and let the circuit reach steady-state.
 *    Then put CHRG/MEAS into input mode, and measure the time it takes for the state to switch.
 *    This time is dependent upon C, Rup, Vcc and Vtoggle only.
 *    We can adjust one (or multiple) of those variables to reflect the actual measurement.
 *    Vtoggle likely has the highest variability, so adjust that one.
 *  We can actually achieve two calibration data:
 *    One charging through only Rup, and the other charging through Rup and Rchrg
 */
class RCThermistor2Pin : public IODriver {
    enum ThermMode {
        MODE_PREPARING,
        MODE_READING,
        MODE_CALIBRATING,
    };
    IoPin thermPin;
    IoPin chargeMeasPin;
    float C, Vcc, Rup, Rchrg, Rseries;
    float Vtoggle;
    //R0 = measured resistance at temperature T0 (in Ohms and Kelvin) (listed on thermistor packaging or documentation page)
    float T0, R0; 
    //Thermistor Beta value; describes how thermistor changes resistance over the temperature range (listed on thermistor packaging or documentation page)
    float B;
    EventClockT::duration readInterval;
    EventClockT::duration readTimeout;
    EventClockT::time_point startModeTime;
    bool isCalibrated;
    ThermMode mode;
    float lastTemp;
    public:
        inline RCThermistor2Pin(IoPin &&thermPin, IoPin &&chargeMeasPin, float RCHRG_OHMS, float RSERIES_OHMS, float RUP_OHMS,
            float C_FARADS, float VCC_V, float V_TOGGLE_V, float T0_C, float R0_OHMS, float BETA, 
            EventClockT::duration readInterval=std::chrono::milliseconds(3000))
          : thermPin(std::move(thermPin)), chargeMeasPin(std::move(chargeMeasPin)), 
            C(C_FARADS), Vcc(VCC_V), Rup(RUP_OHMS), Rchrg(RCHRG_OHMS), Rseries(RSERIES_OHMS),
            Vtoggle(V_TOGGLE_V), T0(mathutil::CtoK(T0_C)), R0(R0_OHMS), B(BETA),
            readInterval(readInterval), 
            readTimeout(this->readInterval.count()/10),
            isCalibrated(false), mode(MODE_PREPARING), lastTemp(mathutil::ABSOLUTE_ZERO_CELCIUS) {
            thermPin.setDefaultState(IO_DEFAULT_HIGH_IMPEDANCE);
            chargeMeasPin.setDefaultState(IO_DEFAULT_HIGH_IMPEDANCE);
            setModePreparing();
        }
        inline bool onIdleCpu(OnIdleCpuIntervalT interval) {
            (void)interval;
            if (mode == MODE_PREPARING) {
                if ((EventClockT::now() - startModeTime) > readInterval) {
                    //only read on a periodic basis because it requires busy-waiting (high cpu usage).
                    if (isCalibrated) {
                        setModeReading();
                    } else {
                        setModeCalibrating();
                    }
                    return true;
                } else {
                    return false;
                }
            } else {
                if (chargeMeasPin.digitalRead() == IoLow) { 
                    //capacitor is still discharging; check for possible timeout
                    //note: when reading, the interval will always be OnIdleCpuIntervalShort, so we don't have an easy way to only check timeout periodically
                    if (/*interval == OnIdleCpuIntervalWide &&*/ EventClockT::now()-startModeTime > readTimeout) {
                        //read has timed out
                        LOG("RCThermistor2Pin read timeout\n");
                        setModePreparing();
                        return false;
                    } else {
                        return true;
                    }
                } else {
                    //reading is complete. Log the current time to determine discharge duration:
                    float duration = std::chrono::duration_cast<std::chrono::duration<float> >(EventClockT::now() - startModeTime).count();
                    if (mode == MODE_READING) {
                        LOGV("time to read resistor: %f\n", duration);
                        //now try to guess the resistance:
                        float resistance = guessRFromTime(duration);
                        LOGV("Resistance guess: %f\n", resistance);
                        lastTemp = temperatureFromR(resistance);
                        LOGV("Temperature guess: %f\n", lastTemp);
                    } else if (mode == MODE_CALIBRATING) {
                        updateValuesFromCalibrationData(duration);
                        isCalibrated = true;
                    }
                    setModePreparing();
                    return false;
                }
            }
        }
        inline float value() const {
            return lastTemp;
        }
    private:
        //bring the capacitor as close to ground as possible to prepare it for a read.
        inline void setModePreparing() {
            mode = MODE_PREPARING;
            startModeTime = EventClockT::now();
            //disconnect thermistor to allow the capacitor to be drained
            thermPin.makeDigitalInput();
            //prepare IOs for the next read (ie. drain the capacitor that was charged during reading)
            chargeMeasPin.makeDigitalOutput(IoLow);
        }
        //after a call to setModePreparing + a sufficient delay,
        //  configure the capacitor to be charged through the thermistor
        inline void setModeReading() {
            mode = MODE_READING;
            //TODO: Also need to ensure that the we haven't been pre-empted between grabbing the time & setting the pins.
            startModeTime = EventClockT::now();
            //disconnect charge pin from ground and use it to measure
            chargeMeasPin.makeDigitalInput();
            //tie thermistor to high to begin drain
            thermPin.makeDigitalOutput(IoHigh);
        }
        inline void setModeCalibrating() {
            mode = MODE_CALIBRATING;
            //TODO: Also need to ensure that the we haven't been pre-empted between grabbing the time & setting the pins.
            startModeTime = EventClockT::now();
            //disconnect charge pin from ground and use it to measure
            chargeMeasPin.makeDigitalInput();
            //disconnect thermistor to drain only through Rup.
            thermPin.makeDigitalInput();
        }
        //@tr the time it took to charge the capacitor through the thermistor
        inline float guessRFromTime(float tr) const {
            /*Derive the resistance as a function of the time taken to charge capacitor
             *First, we want to solve for v(t) across the capacitor:
             *  Step response when switched to read mode:
             *    v(0) = vi = Vcc*Rchrg / (Rup+Rchrg)
             *    v(infinity) = vf = Vcc
             *    Tau = Rread*C where Rread = Rup || (Rseries + therm) = (Rup+Rseries+therm)/(Rup*(Rseries+therm))
             *    v(t) = (vi-vf)*e^(-t/Tau) + vf
             *  knowing v(tr) = Vtoggle:
             *    ln((Vtoggle - vf)/(vi-vf)) = -tr/(Rread*C)
             *   *Rread = -tr/(C*ln((Vtoggle - vf)/(vi-vf)))
             *  Then extract the thermistor value from the total resistance:
             *    Rread = Rup*(Rseries+therm)/(Rup+Rseries+therm)
             *    Rread*(Rup+Rseries+therm) = Rup*(Rseries+therm)
             *    Rread*therm + Rread*(Rseries+Rup) = Rup*therm + Rup*Rseries
             *    therm(Rread-Rup) = Rup*Rseries - Rread*(Rseries+Rup)
             *    *therm = (Rup*Rseries - Rread*(Rseries+Rup)) / (Rread-Rup)
             */
            float vi = Vcc*Rchrg / (Rup+Rchrg);
            float vf = Vcc;
            float Rread = -tr/(C*log((Vtoggle - vf)/(vi-vf)));
            float therm = (Rup*Rseries - Rread*(Rseries+Rup)) / (Rread-Rup);
            return therm;
        }
        inline float temperatureFromR(float R) const {
            if (R < 0) {
                LOGE("RCThermistor2Pin guessRFromTime returned a negative value! (thermistor read error)\n");
                return mathutil::ABSOLUTE_ZERO_CELCIUS;
            }
            float K = 1. / (1./T0 + log(R/R0)/B); //resistance;
            return mathutil::KtoC(K);
        }
        //@tr the time it took to charge the capacitor through the fixed resistance, Rup
        inline void updateValuesFromCalibrationData(float tr) {
            //During a calibration read, in which THERMPIN and CHRG/MEAS are both high-impedance,
            //We have the following relation:
            //  v(0) = vi = Vcc*Rchrg / (Rup+Rchrg)
            //  v(infinity) = vf = Vcc
            //  Tau = Rup*C
            //  thus, v(t) = (vi-vf)*e^(-t/Tau) + vf
            //  knowing that v(tr) SHOULD equal Vtoggle, we will thus adjust our Vtoggle value to math v(tr)
            float Tau = Rup*C;
            float vi = Vcc*Rchrg / (Rup+Rchrg);
            float vf = Vcc;
            Vtoggle = (vi-vf)*exp(-tr/Tau) + vf;
        } 
};


}

#endif