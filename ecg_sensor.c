/*  ECG sensor pindef. 
    Michael Nolan, WCNG University of Rochester
    17/3/2014 (Happy St. Patrick's Day!)
    */

/* See license.txt for license information. */
#include "mywisp.h"
#if (ACTIVE_SENSOR == SENSOR_ACCEL)

#include "dlwisp41.h"
#include "ecg_sensor.h"

#if DEBUG_BAD_SAMPLES
short lastx = 0xffff, lasty = 0xffff, lastz = 0xffff;
short x = 0xffff, y = 0xffff, z = 0xffff;
short diff = 0;
#endif

void init_sensor()
{
        return;
}

#if 1
void read_sensor(unsigned char volatile *target)
{  
        unsigned short enough_power;
  
        if(!is_power_good())
          sleep(); 
        
        P__3__OUT |= SENSOR_POWER_PIN;
        for( i = 0; i < 1000; i++ );    //  wait for the sensor pin to settle

        P1OUT &= ~RX_EN_PIN;   // turn off comparator
     
        // slow down clock
        BCSCTL1 = XT2OFF + RSEL3; 
        DCOCTL = DCO2 + DCO1 + DCO0;
        
        // Power sensor, enable analog in     
        ADC10AE0 |= (X_INCH + Y_INCH + Z_INCH);
        SET_ACCEL_ENABLE_DIR;
        TURN_ON_ACCEL_ENABLE;
        
        // set up watchdog interval timer to sleep during settle time
        WDTCTL = WDT_MDLY_0_5;
        IE1 |= WDTIE;
        P1IE = 0;
        P2IE = 0;
        
        // a little time for regulator to stabilize active mode current AND
        // filter caps to settle. for WDT_MDLY_0_5 * 46, this is slightly less than 10 ms
        for ( int k = 0 ; k < 46 ; k++ )
        {
#if CHECK_FOR_GOOD_VOLTAGE
          //DEBUG_PIN5_HIGH;
          if ( k == 1 )
          {
            //DEBUG_PIN5_HIGH;
            enough_power = is_power_good();
            //DEBUG_PIN5_LOW;
          }
#endif
          _BIS_SR(LPM1_bits+GIE);
          //DEBUG_PIN5_LOW;
        }
        IE1 &= ~WDTIE;

#if CHECK_FOR_GOOD_VOLTAGE
        // make sure there's enough voltage to generate good samples. samples
        // get seriously wacky at ~1.8V, especially the already-noisy Z channel.
        // see the accel data sheet for details.
        if ( ! enough_power )
        {
          // low voltage -- don't sample
          //DEBUG_PIN5_HIGH;
          CLEAR_ACCEL_ENABLE_DIR;
          TURN_OFF_ACCEL_ENABLE;
          ADC10AE0 &= ~(X_INCH + Y_INCH + Z_INCH);
          ADC10CTL0 &= ~ENC;
          ADC10CTL1 = 0;       // turn adc off
          ADC10CTL0 = 0;       // turn adc off
          //DEBUG_PIN5_LOW;
          //return 0;
        }
#endif
        
        // grab data
        for (int i = (DATA_LENGTH_IN_WORDS-1); i >= 0; i--)
        {
        
          ADC10CTL0 &= ~ENC; // make sure this is off otherwise settings are locked.
          ADC10CTL0 = SREF_0 + ADC10SHT_3 + ADC10ON + ADC10IE;
          if (i == 2)
            // sample Z channel first, because it is the most noisy
            ADC10CTL1 = ADC10DIV_4 + ADC10SSEL_0 + SHS_0 + CONSEQ_0 + Z_INCH;
          else if (i == 1)
            ADC10CTL1 = ADC10DIV_4 + ADC10SSEL_0 + SHS_0 + CONSEQ_0 + Y_INCH;
          else
            ADC10CTL1 = ADC10DIV_4 + ADC10SSEL_0 + SHS_0 + CONSEQ_0 + X_INCH;
          ADC10CTL0 |= ENC;
          ADC10CTL0 |= ADC10SC;
          LPM4;
          
          unsigned int k;
          if ( i == 0 ) k = 0;
          else if ( i == 1 ) k = 2;
          else k = 4;
          
          // grab sample and write it to ram
          *(target + k + 1 ) = (ADC10MEM & 0xff);
          *(target + k) = (ADC10MEM & 0x0300) >> 8;
          
#if DEBUG_BAD_SAMPLES
          if ( i == 0 ) x = ADC10MEM;
          else if ( i == 1 ) y = ADC10MEM;
          else z = ADC10MEM;
#endif
            
          k += 2;
        }
        
#if DEBUG_BAD_SAMPLES
//#define THRES 50 // about 5% ... never fires
//#define THRES 30 // about 3% ... fires very rarely
//#define THRES 20 
#define THRES 10 
        
        unsigned short fired = 0;
        diff = ((x - lastx) > 0) ? (x - lastx) : (lastx -x);
        if ( lastx != 0xffff && diff > THRES )
        {
          fired = 1;
        }
        diff = ((y - lasty) > 0) ? (y - lasty) : (lasty -y);
        if ( lasty != 0xffff && diff > THRES )
        {
          fired = 1;
        }
        diff = ((z - lastz) > 0) ? (z- lastz) : (lastz -z);
        if ( lastz != 0xffff && diff > THRES )
        {
          fired = 1;
        }
        lastx = x; lasty = y ; lastz = z;
  
        if ( fired ) { DEBUG_PIN5_HIGH; DEBUG_PIN5_LOW; }
#endif
        
        // Power off sensor and adc
        CLEAR_ACCEL_ENABLE_DIR;
        TURN_OFF_ACCEL_ENABLE;
        ADC10AE0 &= ~(X_INCH + Y_INCH + Z_INCH);
        ADC10CTL0 &= ~ENC;
        ADC10CTL1 = 0;       // turn adc off
        ADC10CTL0 = 0;       // turn adc off
         
        //return 1;
}
#endif

#if 0
void read_sensor(unsigned char volatile *target) 
{
        static short cntr = 0;
        
        if(!is_power_good())
          sleep(); 
        
        P1OUT &= ~RX_EN_PIN;   // turn off comparator
        
        // set up watchdog interval timer to sleep during settle time
        WDTCTL = WDT_MDLY_0_5;
        IE1 |= WDTIE;
        
        // a little time for regulator to stabilize active mode current AND
        // filter caps to settle. for WDT_MDLY_0_5 * 48, this is 10 ms
        DEBUG_PIN5_HIGH;
        for ( int k = 0 ; k < 48 ; k++ )
        {
          _BIS_SR(LPM1_bits+GIE);
        }
        DEBUG_PIN5_LOW;
        
        IE1 &= ~WDTIE;
        
        if ( !is_power_good() )
        {
          DEBUG_PIN5_HIGH;
          DEBUG_PIN5_LOW;
          //return 0;
        }
        
//#define DELTA   0x05
#define DELTA   0x0B
//#define DELTA   0x14
        
#define HIBYTE      0x01
//#define HIBYTE      0x02

        *(target) = HIBYTE;
        *(target + 1 ) = 0xF8;
        *(target + 2) = HIBYTE;
        *(target + 3 ) = 0xF3;
        *(target + 4) = HIBYTE;
        *(target + 5 ) = 0xCA;
        
        if ( cntr++ == 2 )
        {
          *(target + 1 ) = 0xF8 - DELTA; 
           *(target + 3 ) = 0xF3 - DELTA;
           *(target + 5 ) = 0xCA + DELTA; 
           cntr = 0;
        }

        
        
        
        
        //return 1;
}
#endif

// used to sleep while taking adc, this wakes us
#pragma vector=ADC10_VECTOR
__interrupt void ADC10_ISR (void)
{
      ADC10CTL0 &= ~ENC; // make sure this is off otherwise settings are locked.
      ADC10CTL0 &= ~(ADC10IFG | ADC10IE);
      LPM4_EXIT;
      return;
}

#pragma vector=WDT_VECTOR
__interrupt void wdt_ISR( void )
{
      LPM4_EXIT;
      return;
}

#endif // (ACTIVE_SENSOR == SENSOR_ACCEL)
