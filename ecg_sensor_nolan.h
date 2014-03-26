/*  Created 25/3/2014 by Michael Nolan. Creating a WISP-ECG monitor interface.  */

#ifndef ECG_SENSOR_NOLAN_H
#define ECG_SENSOR_NOLAN_H


//  bit definitions specific to the WISP 4.1 DL

#define SENSOR_DATA_TYPE_ID     0X0B

#define ACCEL_ENABLE_BIT            BITS    //  1.5
#define SET_ACCEL_ENABLE_DIR        P1DIR |= ACCEL_ENABLE_BIT
#define CLEAR_ACCEL_ENABLE_DIR    PIDIR &= ~ACCEL_ENABLE_BIT
#define TURN_ON_ACCEL_ENABLE  P1OUT |= ACCEL_ENABLE_BIT
#define TURN_OFF_ACCEL_ENABLE    P1OUT &= ~ACCEL_ENABLE_BIT

#define DATA_LENGTH_IN_WORDS        3
#define DATA_LENGTH_IN_BYTES        (DATA_LENGTH_IN_WORDS*2)

#endif  //  ECG_SENSOR_NOLAN_H
