/* See license.txt for license information. */

//******************************************************************************
//  WISP 4.1
//  Device: MSP430-2132
//
//  Version HW4.1_SW6.0
//
//  Pinout:
//             P1.1 = Data out signal
//             P1.2 = Data in signal
//             P1.3 = Data in enable
//
//             P2.4 = Supervisor In
//             P2.5 = Supervisor In
//
//         Tested against Impinj v 3.0.2 reader, Miller-4 encodings
//         Alien reader code is out of date
//
//  This is a partial implementation of the RFID EPCGlobal C1G2 protocol
//  for the WISP 4.1 hardware platform.
//
//  What's missing:
//        - SECURED and KILLED states.
//        - No support for WRITE, KILL, LOCK, ACCESS, BLOCKWRITE and BLOCKERASE
//          commands.
//        - Commands with EBVs always assume that the field is 8 bits long.
//        - SELECTS don't support truncation.
//        - READs ignore membank, wordptr, and wordcount fields. (What READs do
//          return is dependent on what application you have configured in step
//          1.)
//        - I sometimes get erroneous-looking session values in QUERYREP
//          commands.  For the time being, I just parse the command as if the
//          session value is the same as my previous_session value.
//        - QUERYs use a pretty aggressive slotting algorithm to preserve power.
//          See the comments in that function for details.
//        - Session timeouts work differently than what's in table 6.15 of the
//          spec.  Here's what we do:
//            SL comes up as not asserted, and persists as long as it's in ram
//            retention mode or better.
//            All inventory flags come up as 'A'. S0's inventory flag persists
//            as long as it's in ram retention mode or better. S1's inventory
//            flag persists as long as it's in an inventory round and in ram
//            retention mode or better; otherwise it is reset to 'A' with every
//            reset at the top of the while loop. S2's and S3's inventory flag
//            is reset to 'A' with every reset at the top of the while loop.
//******************************************************************************

#if(WISP_VERSION != BLUE_WISP)
  #error "WISP Version not supported"
#endif
#include "rfid.h"

/*******************************************************************************
 *****************  Edit mywisp.h to configure this WISP  **********************
 ******************************************************************************/
#include "mywisp.h"

// as per mapping in monitor code
#define wisp_debug_1                  DEBUG_1_4   // P1.4
#define wisp_debug_2                  DEBUG_2_3   // P2.3
#define wisp_debug_3                  CLK_A       // P3.0
#define wisp_debug_4                  TX_A        // P3.4
#define wisp_debug_5                  RX_A        // P3.5
#define MONITOR_DEBUG_ON                 0

volatile unsigned char* destorig = &cmd[0];       // pointer to beginning of cmd

// #pragma data_alignment=2 is important in sendResponse() when the words are
// copied into arrays.  Sometimes the compiler puts reply[0] on an odd address,
// which cannot be copied as a word and thus screws everything up.
#pragma data_alignment=2

// compiler uses working register 4 as a global variable
// Pointer to &cmd[bits]
volatile __no_init __regvar unsigned char* dest @ 4;

// compiler uses working register 5 as a global variable
// count of bits received from reader
volatile __no_init __regvar unsigned short bits @ 5;
unsigned short TRcal=0;

#if ENABLE_SESSIONS
// selected and session inventory flags
#define S0_INDEX        0x00
#define S1_INDEX        0x01
#define S2_INDEX        0x02
#define S3_INDEX        0x03

#define SL_ASSERTED     1
#define SL_NOT_ASSERTED     0
#define SESSION_STATE_A     0
#define SESSION_STATE_B     1

unsigned char SL;
unsigned char previous_session = 0x00;
unsigned char session_table[] = {
    SESSION_STATE_A, SESSION_STATE_A,
    SESSION_STATE_A, SESSION_STATE_A
};
#endif // ENABLE_SESSIONS
int i;

int main(void)
{
  //*******************************Timer setup**********************************
  WDTCTL = WDTPW + WDTHOLD;            // Stop Watchdog Timer

  P1SEL = 0;
  P2SEL = 0;

  P1IE = 0;
  P1IFG = 0;
  P2IFG = 0;

  DRIVE_ALL_PINS // set pin directions correctly and outputs to low.

  // Check power on bootup, decide to receive or sleep.
  if(!is_power_good())
    sleep();

  RECEIVE_CLOCK;

  /*
  // already set.
#if MONITOR_DEBUG_ON
    // for monitor - pin direction
  P1DIR |= wisp_debug_1;
  P2DIR |= wisp_debug_2;
  P3DIR |= wisp_debug_3;
  P3DIR |= wisp_debug_4;
  P3DIR |= wisp_debug_5;
#endif
  */

#if DEBUG_PINS_ENABLED
#if USE_2132
  DEBUG_PIN5_LOW;
//  P3DIR |= (BIT5); // already set.
#else
  P4DIR |= (BIT7+BIT5+BIT3);
#endif
#endif

#if ENABLE_SLOTS
  // setup int epc
  epc = ackReply[2]<<8;
  epc |= ackReply[3];

  // calculate RN16_1 table
  for (Q = 0; Q < 16; Q++)
  {
    rn16 = epc^Q;
    lfsr();

    if (Q > 8)
    {
      RN16[(Q<<1)-9] = __swap_bytes(rn16);
      RN16[(Q<<1)-8] = rn16;
    }
    else
    {
      RN16[Q] = rn16;
    }
  }
#endif

  TACTL = 0;

//  P1IES &= ~BIT2; // initial state is POS edge to find start of CW
//  P1IFG = 0x00;       // clear interrupt flag after changing edge trigger

  asm("MOV #0000h, R9");
  // dest = destorig;

#if READ_SENSOR
  init_sensor();
#endif

#if !(ENABLE_SLOTS)
  queryReplyCRC = crc16_ccitt(&queryReply[0],2);
  queryReply[3] = (unsigned char)queryReplyCRC;
  queryReply[2] = (unsigned char)__swap_bytes(queryReplyCRC);
#endif

#if SENSOR_DATA_IN_ID
  // this branch is for sensor data in the id
  ackReply[2] = SENSOR_DATA_TYPE_ID;
  state = STATE_READ_SENSOR;
  timeToSample++;
#else
  ackReplyCRC = crc16_ccitt(&ackReply[0], 14);
  ackReply[15] = (unsigned char)ackReplyCRC;
  ackReply[14] = (unsigned char)__swap_bytes(ackReplyCRC);
#endif

#if ENABLE_SESSIONS
  initialize_sessions();
#endif

  //state = STATE_ARBITRATE;
  state = STATE_READY;

#if MONITOR_DEBUG_ON
  // for monitor - set STATE READY debug line - 00000 - 0
  P1OUT |= wisp_debug_1;
  P1OUT &= ~wisp_debug_1;
  P2OUT &= ~wisp_debug_2;
#endif

  setup_to_receive();

  while (1)
  {

    // TIMEOUT!  reset timer
    if (TAR > 0x256 || delimiterNotFound)   // was 0x1000
    {
      if(!is_power_good()) {
        sleep();
      }

#if MONITOR_DEBUG_ON
      // for monitor - set TAR OVERFLOW debug line - 00111 - 7
      if (!delimiterNotFound)
      {
        P1OUT |= wisp_debug_1;
        P1OUT &= ~wisp_debug_1;
        P2OUT &= ~wisp_debug_2;
        P3OUT = 0x31;
      }
#endif

#if SENSOR_DATA_IN_ID
    // this branch is for sensor data in the id
      if ( timeToSample++ == 10 ) {
        state = STATE_READ_SENSOR;
        timeToSample = 0;
      }
#elif SENSOR_DATA_IN_READ_COMMAND
      if ( timeToSample++ == 10 ) {
        state = STATE_READ_SENSOR;
        timeToSample = 0;
      }
#else
#if !(ENABLE_READS)
    if(!is_power_good())
        sleep();
#endif
    inInventoryRound = 0;
    state = STATE_READY;

#if MONITOR_DEBUG_ON
    // for monitor - set STATE READY debug line - 00000 - 0
    P1OUT |= wisp_debug_1;
    P1OUT &= ~wisp_debug_1;
    P2OUT &= ~wisp_debug_2;
#endif

#endif

#if ENABLE_SESSIONS
    handle_session_timeout();
#endif

#if ENABLE_SLOTS
    if (shift < 4)
        shift += 1;
    else
        shift = 0;
#endif

      setup_to_receive();
    }

    switch (state)
    {
      case STATE_READY:
      {
        inInventoryRound = 0;
        //////////////////////////////////////////////////////////////////////
        // process the QUERY command
        //////////////////////////////////////////////////////////////////////
        if ( bits == NUM_QUERY_BITS  && ( ( cmd[0] & 0xF0 ) == 0x80 ) )
        {
#if MONITOR_DEBUG_ON
            // for monitor - set QUERY PKT debug line - 01001 - 9
          P1OUT |= wisp_debug_1;
          P1OUT &= ~wisp_debug_1;
          P2OUT |= wisp_debug_2;
          P3OUT = 0x01;
#endif

          //DEBUG_PIN5_HIGH;
          handle_query(STATE_REPLY);

#if MONITOR_DEBUG_ON
          // for monitor - set STATE REPLY debug line - 00010 - 2
          P1OUT |= wisp_debug_1;
          P1OUT &= ~wisp_debug_1;
          P2OUT &= ~wisp_debug_2;
          P3OUT = 0x10;
#endif

          setup_to_receive();
        }
        //////////////////////////////////////////////////////////////////////
        // process the SELECT command
        //////////////////////////////////////////////////////////////////////
        // @ short distance has slight impact on performance
        else if ( bits >= 44  && ( ( cmd[0] & 0xF0 ) == 0xA0 ) )
        {

#if MONITOR_DEBUG_ON
          // for monitor - set QUERY_ADJ debug line - 01101 - 13
          P1OUT |= wisp_debug_1;
          P1OUT &= ~wisp_debug_1;
          P2OUT |= wisp_debug_2;
          P3OUT = 0x30;
#endif
          //DEBUG_PIN5_HIGH;
          handle_select(STATE_READY);
          //DEBUG_PIN5_LOW;
          delimiterNotFound = 1;
        } // select command
        //////////////////////////////////////////////////////////////////////
        // got >= 22 bits, and it's not the beginning of a select. just reset.
        //////////////////////////////////////////////////////////////////////
        else if ( bits >= MAX_NUM_QUERY_BITS && ( ( cmd[0] & 0xF0 ) != 0xA0 ) )
        {
          do_nothing();
          state = STATE_READY;
          delimiterNotFound = 1;

#if MONITOR_DEBUG_ON
          // for monitor - set debug line - 01110 - 14
          P1OUT |= wisp_debug_1;
          P1OUT &= ~wisp_debug_1;
          P2OUT &= ~wisp_debug_2;
          P3OUT = 0x30;
#endif
        }
        break;
      }
      case STATE_ARBITRATE:
      {
        //////////////////////////////////////////////////////////////////////
        // process the QUERY command
        //////////////////////////////////////////////////////////////////////
        if ( bits == NUM_QUERY_BITS  && ( ( cmd[0] & 0xF0 ) == 0x80 ) )
        {
#if MONITOR_DEBUG_ON
          // for monitor - set QUERY PKT debug line - 01001 - 9
          P1OUT |= wisp_debug_1;
          P1OUT &= ~wisp_debug_1;
          P2OUT |= wisp_debug_2;
          P3OUT = 0x01;
#endif

          //DEBUG_PIN5_HIGH;
          handle_query(STATE_REPLY);

#if MONITOR_DEBUG_ON
          // for monitor - set STATE REPLY debug line - 00010 - 2
          P1OUT |= wisp_debug_1;
          P1OUT &= ~wisp_debug_1;
          P2OUT &= ~wisp_debug_2;
          P3OUT = 0x10;
#endif

          setup_to_receive();
        }
        //////////////////////////////////////////////////////////////////////
        // got >= 22 bits, and it's not the beginning of a select. just reset.
        //////////////////////////////////////////////////////////////////////
        //else if ( bits >= NUM_QUERY_BITS )
        else if ( bits >= MAX_NUM_QUERY_BITS && ( ( cmd[0] & 0xF0 ) != 0xA0 ) )
        {
          //DEBUG_PIN5_HIGH;
          do_nothing();
          state = STATE_READY;
          delimiterNotFound = 1;

#if MONITOR_DEBUG_ON
          // for monitor - set DELIMITER NOT FOUND debug line - 00110 - 6
          P1OUT |= wisp_debug_1;
          P1OUT &= ~wisp_debug_1;
          P2OUT &= ~wisp_debug_2;
          P3OUT = 0x30;
#endif

          //DEBUG_PIN5_LOW;
        }
        // this state handles query, queryrep, queryadjust, and select commands.
        //////////////////////////////////////////////////////////////////////
        // process the QUERYREP command
        //////////////////////////////////////////////////////////////////////
        else if ( bits == NUM_QUERYREP_BITS && ( ( cmd[0] & 0x06 ) == 0x00 ) )
        {
#if MONITOR_DEBUG_ON
          // for monitor - set QUERY_REP debug line - 01100 - 12
          P1OUT |= wisp_debug_1;
          P1OUT &= ~wisp_debug_1;
          P2OUT |= wisp_debug_2;
          P3OUT = 0x20;
#endif
          //DEBUG_PIN5_HIGH;
          handle_queryrep(STATE_REPLY);
          //DEBUG_PIN5_LOW;
          //setup_to_receive();
          delimiterNotFound = 1;

#if MONITOR_DEBUG_ON
          // for monitor - set DELIMITER NOT FOUND debug line - 00110 - 6
          P1OUT |= wisp_debug_1;
          P1OUT &= ~wisp_debug_1;
          P2OUT &= ~wisp_debug_2;
          P3OUT = 0x30;
#endif
        } // queryrep command
        //////////////////////////////////////////////////////////////////////
        // process the QUERYADJUST command
        //////////////////////////////////////////////////////////////////////
        else if ( bits == NUM_QUERYADJ_BITS  && ( ( cmd[0] & 0xF8 ) == 0x48 ) )
        {
          // at short distance, you get better performance (~52 t/s) if you
          // do setup_to_receive() rather than dnf =1. not sure that this holds
          // true at distance though - need to recheck @ 2-3 ms.
          //DEBUG_PIN5_HIGH;

#if MONITOR_DEBUG_ON
          // for monitor - set QUERY_ADJ debug line - 01101 - 13
          P1OUT |= wisp_debug_1;
          P1OUT &= ~wisp_debug_1;
          P2OUT |= wisp_debug_2;
          P3OUT = 0x21;
#endif

          handle_queryadjust(STATE_REPLY);
          //DEBUG_PIN5_LOW;
          setup_to_receive();
          //delimiterNotFound = 1;
        } // queryadjust command
        //////////////////////////////////////////////////////////////////////
        // process the SELECT command
        //////////////////////////////////////////////////////////////////////
        // @ short distance has slight impact on performance
        else if ( bits >= 44  && ( ( cmd[0] & 0xF0 ) == 0xA0 ) )
        {
#if MONITOR_DEBUG_ON
          // for monitor - set SELECT debug line - 01110 - 14
          P1OUT |= wisp_debug_1;
          P1OUT &= ~wisp_debug_1;
          P2OUT |= wisp_debug_2;
          P3OUT = 0x30;
#endif

          //DEBUG_PIN5_HIGH;
          handle_select(STATE_READY);
          //DEBUG_PIN5_LOW;
          delimiterNotFound = 1;

#if MONITOR_DEBUG_ON
          // for monitor - set DELIMITER NOT FOUND debug line - 00110 - 6
          P1OUT |= wisp_debug_1;
          P1OUT &= ~wisp_debug_1;
          P2OUT &= ~wisp_debug_2;
          P3OUT = 0x30;
#endif

        } // select command

      break;
      }

      case STATE_REPLY:
      {
        // this state handles query, query adjust, ack, and select commands
        ///////////////////////////////////////////////////////////////////////
        // process the ACK command
        ///////////////////////////////////////////////////////////////////////
        if ( bits == NUM_ACK_BITS  && ( ( cmd[0] & 0xC0 ) == 0x40 ) )
        {
#if MONITOR_DEBUG_ON
          // for monitor - set ACK PKT debug line - 01010 - 10
          P1OUT |= wisp_debug_1;
          P1OUT &= ~wisp_debug_1;
          P2OUT |= wisp_debug_2;
          P3OUT = 0x10;
#endif
#if ENABLE_READS
          handle_ack(STATE_ACKNOWLEDGED);

          setup_to_receive();
#elif SENSOR_DATA_IN_ID
          handle_ack(STATE_ACKNOWLEDGED);
          delimiterNotFound = 1; // reset
#else
          // this branch for hardcoded query/acks
          handle_ack(STATE_ACKNOWLEDGED);
          //delimiterNotFound = 1; // reset
          setup_to_receive();
#endif
#if MONITOR_DEBUG_ON
          // for monitor - set STATE ACK debug line - 00011 - 3
          P1OUT |= wisp_debug_1;
          P1OUT &= ~wisp_debug_1;
          P2OUT &= ~wisp_debug_2;
          P3OUT = 0x11;
#endif
        }
        //////////////////////////////////////////////////////////////////////
        // process the QUERY command
        //////////////////////////////////////////////////////////////////////
        if ( bits == NUM_QUERY_BITS  && ( ( cmd[0] & 0xF0 ) == 0x80 ) )
        {
#if MONITOR_DEBUG_ON
          // for monitor - set QUERY PKT debug line - 01001 - 9
          P1OUT |= wisp_debug_1;
          P1OUT &= ~wisp_debug_1;
          P2OUT |= wisp_debug_2;
          P3OUT = 0x01;
#endif
          // i'm supposed to stay in state_reply when I get this, but if I'm
          // running close to 1.8v then I really need to reset and get in the
          // sleep, which puts me back into state_arbitrate. this is complete
          // a violation of the protocol, but it sure does make everything
          // work better. - polly 8/9/2008
          //DEBUG_PIN5_HIGH;
          handle_query(STATE_REPLY);
          //DEBUG_PIN5_LOW;
          //delimiterNotFound = 1;
          setup_to_receive();
        }
        //////////////////////////////////////////////////////////////////////
        // process the QUERYREP command
        //////////////////////////////////////////////////////////////////////
        else if ( bits == NUM_QUERYREP_BITS && ( ( cmd[0] & 0x06 ) == 0x00 ) )
        {
#if MONITOR_DEBUG_ON
          // for monitor - set QUERY_REP debug line - 01100 - 12
          P1OUT |= wisp_debug_1;
          P1OUT &= ~wisp_debug_1;
          P2OUT |= wisp_debug_2;
          P3OUT = 0x20;
#endif

          //DEBUG_PIN5_HIGH;
      do_nothing();
      state = STATE_ARBITRATE;

#if MONITOR_DEBUG_ON
          // for monitor - set STATE ARBITRATE debug line - 00001 - 1
          P1OUT |= wisp_debug_1;
          P1OUT &= ~wisp_debug_1;
          P2OUT &= ~wisp_debug_2;
          P3OUT = 0x01;
#endif

          //handle_queryrep(STATE_ARBITRATE);
          //DEBUG_PIN5_LOW;
          setup_to_receive();
          //delimiterNotFound = 1; // reset
        } // queryrep command
        //////////////////////////////////////////////////////////////////////
        // process the QUERYADJUST command
        //////////////////////////////////////////////////////////////////////
          else if (bits == NUM_QUERYADJ_BITS && ( ( cmd[0] & 0xF8 ) == 0x48 ))
        {
#if MONITOR_DEBUG_ON
          // for monitor - set QUERY_ADJ debug line - 01101 - 13
          P1OUT |= wisp_debug_1;
          P1OUT &= ~wisp_debug_1;
          P2OUT |= wisp_debug_2;
          P3OUT = 0x21;
#endif
          //DEBUG_PIN5_HIGH;
          handle_queryadjust(STATE_REPLY);
          //DEBUG_PIN5_LOW;
          //setup_to_receive();
          delimiterNotFound = 1;

#if MONITOR_DEBUG_ON
          // for monitor - set DELIMITER NOT FOUND debug line - 00110 - 6
          P1OUT |= wisp_debug_1;
          P1OUT &= ~wisp_debug_1;
          P2OUT &= ~wisp_debug_2;
          P3OUT = 0x30;
#endif

        } // queryadjust command
        //////////////////////////////////////////////////////////////////////
        // process the SELECT command
        //////////////////////////////////////////////////////////////////////
        else if ( bits >= 44  && ( ( cmd[0] & 0xF0 ) == 0xA0 ) )
        {
#if MONITOR_DEBUG_ON
          // for monitor - set SELECT debug line - 01110 - 14
          P1OUT |= wisp_debug_1;
          P1OUT &= ~wisp_debug_1;
          P2OUT |= wisp_debug_2;
          P3OUT = 0x30;
#endif

          //DEBUG_PIN5_HIGH;
          handle_select(STATE_READY);
          //DEBUG_PIN5_LOW;
          delimiterNotFound = 1;

#if MONITOR_DEBUG_ON
          // for monitor - set DELIMITER NOT FOUND debug line - 00110 - 6
          P1OUT |= wisp_debug_1;
          P1OUT &= ~wisp_debug_1;
          P2OUT &= ~wisp_debug_2;
          P3OUT = 0x30;
#endif

          //setup_to_receive();
        } // select command
        else if ( bits >= MAX_NUM_QUERY_BITS && ( ( cmd[0] & 0xF0 ) != 0xA0 ) &&
                ( ( cmd[0] & 0xF0 ) != 0x80 ) )
        {
          //DEBUG_PIN5_HIGH;
          do_nothing();
          state = STATE_READY;
          delimiterNotFound = 1;
          //DEBUG_PIN5_LOW;
        }
        break;
      }
      case STATE_ACKNOWLEDGED:
      {
        // responds to query, ack, request_rn cmds
        // takes action on queryrep, queryadjust, and select cmds
        /////////////////////////////////////////////////////////////////////
        // process the REQUEST_RN command
        //////////////////////////////////////////////////////////////////////
        if ( bits >= NUM_REQRN_BITS && ( cmd[0] == 0xC1 ) )
        {
#if 1
#if MONITOR_DEBUG_ON
          // for monitor - set REQ_RN debug line - 01011 - 11
    P1OUT |= wisp_debug_1;
    P1OUT &= ~wisp_debug_1;
    P2OUT |= wisp_debug_2;
    P3OUT = 0x11;
#endif
          DEBUG_PIN5_HIGH;
          handle_request_rn(STATE_OPEN);
          DEBUG_PIN5_LOW;
          setup_to_receive();
#else
          handle_request_rn(STATE_READY);
          delimiterNotFound = 1;

#if MONITOR_DEBUG_ON
          // for monitor - set DELIMITER NOT FOUND debug line - 00110 - 6
          P1OUT |= wisp_debug_1;
          P1OUT &= ~wisp_debug_1;
          P2OUT &= ~wisp_debug_2;
          P3OUT = 0x30;
#endif
#endif
        }

#if 1
        //////////////////////////////////////////////////////////////////////
        // process the QUERY command
        //////////////////////////////////////////////////////////////////////
        if ( bits == NUM_QUERY_BITS  && ( ( cmd[0] & 0xF0 ) == 0x80 ) )
        {
#if MONITOR_DEBUG_ON
              // for monitor - set QUERY PKT debug line - 01001 - 9
    P1OUT |= wisp_debug_1;
    P1OUT &= ~wisp_debug_1;
    P2OUT |= wisp_debug_2;
    P3OUT = 0x01;
#endif
          //DEBUG_PIN5_HIGH;
          handle_query(STATE_REPLY);
          //DEBUG_PIN5_LOW;
          delimiterNotFound = 1;

#if MONITOR_DEBUG_ON
          // for monitor - set DELIMITER NOT FOUND debug line - 00110 - 6
          P1OUT |= wisp_debug_1;
          P1OUT &= ~wisp_debug_1;
          P2OUT &= ~wisp_debug_2;
          P3OUT = 0x30;
#endif

          //setup_to_receive();
        }
        ///////////////////////////////////////////////////////////////////////
        // process the ACK command
        ///////////////////////////////////////////////////////////////////////
        // this code doesn't seem to get exercised in the real world. if i ever
        // ran into a reader that generated an ack in an acknowledged state,
        // this code might need some work.
        //else if ( bits == 20  && ( ( cmd[0] & 0xC0 ) == 0x40 ) )
        else if ( bits == NUM_ACK_BITS  && ( ( cmd[0] & 0xC0 ) == 0x40 ) )
        {
#if MONITOR_DEBUG_ON
              // for monitor - set ACK PKT debug line - 01010 - 10
    P1OUT |= wisp_debug_1;
    P1OUT &= ~wisp_debug_1;
    P2OUT |= wisp_debug_2;
    P3OUT = 0x10;
#endif
          //DEBUG_PIN5_HIGH;
          handle_ack(STATE_ACKNOWLEDGED);

#if MONITOR_DEBUG_ON
              // for monitor - set STATE ACK debug line - 00011 - 3
    P1OUT |= wisp_debug_1;
    P1OUT &= ~wisp_debug_1;
    P2OUT &= ~wisp_debug_2;
    P3OUT = 0x11;
#endif

          //DEBUG_PIN5_LOW;
          setup_to_receive();
        }
        //////////////////////////////////////////////////////////////////////
        // process the QUERYREP command
        //////////////////////////////////////////////////////////////////////
        else if ( bits == NUM_QUERYREP_BITS && ( ( cmd[0] & 0x06 ) == 0x00 ) )
        {
          // in the acknowledged state, rfid chips don't respond to queryrep
          // commands
          do_nothing();
          state = STATE_READY;
          delimiterNotFound = 1;
        } // queryrep command

        //////////////////////////////////////////////////////////////////////
        // process the QUERYADJUST command
        //////////////////////////////////////////////////////////////////////
        else if ( bits == NUM_QUERYADJ_BITS  && ( ( cmd[0] & 0xF8 ) == 0x48 ) )
        {
          //DEBUG_PIN5_HIGH;
          do_nothing();
          state = STATE_READY;
          delimiterNotFound = 1;
          //DEBUG_PIN5_LOW;
        } // queryadjust command
        //////////////////////////////////////////////////////////////////////
        // process the SELECT command
        //////////////////////////////////////////////////////////////////////
        else if ( bits >= 44  && ( ( cmd[0] & 0xF0 ) == 0xA0 ) )
        {
          //DEBUG_PIN5_HIGH;
          handle_select(STATE_READY);
          delimiterNotFound = 1;
          //DEBUG_PIN5_LOW;
        } // select command
        //////////////////////////////////////////////////////////////////////
        // process the NAK command
        //////////////////////////////////////////////////////////////////////
        else if ( bits >= 10 && ( cmd[0] == 0xC0 ) )
        //else if ( bits >= NUM_NAK_BITS && ( cmd[0] == 0xC0 ) )
        {
          //DEBUG_PIN5_HIGH;
          do_nothing();
          state = STATE_ARBITRATE;
          delimiterNotFound = 1;
          //DEBUG_PIN5_LOW;
        }
        //////////////////////////////////////////////////////////////////////
        // process the READ command
        //////////////////////////////////////////////////////////////////////
        // warning: won't work for read addrs > 127d
        if ( bits == NUM_READ_BITS && ( cmd[0] == 0xC2 ) )
        {
          //DEBUG_PIN5_HIGH;
          handle_read(STATE_ARBITRATE);
          state = STATE_ARBITRATE;
          delimiterNotFound = 1 ;
          //DEBUG_PIN5_LOW;
        }
        // FIXME: need write, kill, lock, blockwrite, blockerase
        //////////////////////////////////////////////////////////////////////
        // process the ACCESS command
        //////////////////////////////////////////////////////////////////////
        if ( bits >= 56  && ( cmd[0] == 0xC6 ) )
        {
          //DEBUG_PIN5_HIGH;
          do_nothing();
          state = STATE_ARBITRATE;
          delimiterNotFound = 1 ;
          //DEBUG_PIN5_LOW;
        }
#endif
        else if ( bits >= MAX_NUM_READ_BITS )
        {
          //DEBUG_PIN5_HIGH;
          //do_nothing();
          state = STATE_ARBITRATE;
          delimiterNotFound = 1 ;
          //DEBUG_PIN5_LOW;
        }

#if 0
        // kills performance ...
        else if ( bits >= 44 )
        {
          do_nothing();
          state = STATE_ARBITRATE;
          delimiterNotFound = 1;
        }
#endif
        break;
      }
      case STATE_OPEN:
      {
        // responds to query, ack, req_rn, read, write, kill, access,
        // blockwrite, and blockerase cmds
        // processes queryrep, queryadjust, select cmds
        //////////////////////////////////////////////////////////////////////
        // process the READ command
        //////////////////////////////////////////////////////////////////////
        // warning: won't work for read addrs > 127d
        if ( bits == NUM_READ_BITS  && ( cmd[0] == 0xC2 ) )
        {
          //DEBUG_PIN5_HIGH;
          handle_read(STATE_OPEN);
          //DEBUG_PIN5_LOW;
          // note: setup_to_receive() et al handled in handle_read
        }
        //////////////////////////////////////////////////////////////////////
        // process the REQUEST_RN command
        //////////////////////////////////////////////////////////////////////
        else if ( bits >= NUM_REQRN_BITS  && ( cmd[0] == 0xC1 ) )
          //else if ( bits >= 30  && ( cmd[0] == 0xC1 ) )
        {
          handle_request_rn(STATE_OPEN);
          setup_to_receive();
          //delimiterNotFound = 1; // more bad reads??
         }
        //////////////////////////////////////////////////////////////////////
        // process the QUERY command
        //////////////////////////////////////////////////////////////////////
        if ( bits == NUM_QUERY_BITS  && ( ( cmd[0] & 0xF0 ) == 0x80 ) )
        {
          handle_query(STATE_REPLY);
          //setup_to_receive();
          delimiterNotFound = 1;
        }
        //////////////////////////////////////////////////////////////////////
        // process the QUERYREP command
        //////////////////////////////////////////////////////////////////////
        else if ( bits == NUM_QUERYREP_BITS && ( ( cmd[0] & 0x06 ) == 0x00 ) )
        {
          //DEBUG_PIN5_HIGH;
          do_nothing();
          state = STATE_READY;
          //delimiterNotFound = 1;
          setup_to_receive();
          //DEBUG_PIN5_LOW;
        } // queryrep command
        //////////////////////////////////////////////////////////////////////
        // process the QUERYADJUST command
        //////////////////////////////////////////////////////////////////////
        //else if ( bits == NUM_QUERYADJ_BITS && ( ( cmd[0] & 0xF0 ) == 0x90 ) )
          else if ( bits == 9  && ( ( cmd[0] & 0xF8 ) == 0x48 ) )
        {
          //DEBUG_PIN5_HIGH;
          do_nothing();
          state = STATE_READY;
          delimiterNotFound = 1;
          //DEBUG_PIN5_LOW;
        } // queryadjust command
        ///////////////////////////////////////////////////////////////////////
        // process the ACK command
        ///////////////////////////////////////////////////////////////////////
        else if ( bits == NUM_ACK_BITS  && ( ( cmd[0] & 0xC0 ) == 0x40 ) )
        {
          //DEBUG_PIN5_HIGH;
          handle_ack(STATE_OPEN);
          //setup_to_receive();
          delimiterNotFound = 1;
          //DEBUG_PIN5_LOW;
        }
        //////////////////////////////////////////////////////////////////////
        // process the SELECT command
        //////////////////////////////////////////////////////////////////////
        else if ( bits >= 44  && ( ( cmd[0] & 0xF0 ) == 0xA0 ) )
        {
          handle_select(STATE_READY);
          delimiterNotFound = 1;
        } // select command
        //////////////////////////////////////////////////////////////////////
        // process the NAK command
        //////////////////////////////////////////////////////////////////////
        //else if ( bits >= NUM_NAK_BITS && ( cmd[0] == 0xC0 ) )
        else if ( bits >= 10 && ( cmd[0] == 0xC0 ) )
        {
          handle_nak(STATE_ARBITRATE);
          delimiterNotFound = 1;
        }

        break;
      }

    case STATE_READ_SENSOR:
      {
#if SENSOR_DATA_IN_READ_COMMAND
        read_sensor(&readReply[0]);
        // crc is computed in the read state
        RECEIVE_CLOCK;
        state = STATE_READY;
        delimiterNotFound = 1; // reset
#elif SENSOR_DATA_IN_ID
        read_sensor(&ackReply[3]);
        RECEIVE_CLOCK;
        ackReplyCRC = crc16_ccitt(&ackReply[0], 14);
        ackReply[15] = (unsigned char)ackReplyCRC;
        ackReply[14] = (unsigned char)__swap_bytes(ackReplyCRC);
        state = STATE_READY;
        delimiterNotFound = 1; // reset
#endif

        break;
      } // end case
    } // end switch

  } // while loop
}

//************************** SETUP TO RECEIVE  *********************************
// note: port interrupt can also reset, but it doesn't call this function
//       because function call causes PUSH instructions prior to bit read
//       at beginning of interrupt, which screws up timing.  so, remember
//       to change things in both places.
inline void setup_to_receive()
{
  //P4OUT &= ~BIT3;
  _BIC_SR(GIE); // temporarily disable GIE so we can sleep and enable interrupts
                // at the same time

  P1OUT |= RX_EN_PIN;

  delimiterNotFound = 0;
  // setup port interrupt on pin 1.2
  P1SEL &= ~BIT2;  //Disable TimerA2, so port interrupt can be used
  // Setup timer. It has to setup because there is no setup time after done with
  // port1 interrupt.
  TACTL = 0;
  TAR = 0;
  TACCR0 = 0xFFFF;    // Set up TimerA0 register as Max
  TACCTL0 = 0;
  TACCTL1 = SCS + CAP;   //Synchronize capture source and capture mode
  TACTL = TASSEL1 + MC1 + TAIE;  // SMCLK and continuous mode and Timer_A
                                 // interrupt enabled.

  // initialize bits
  bits = 0;
  // initialize dest
  dest = destorig;  // = &cmd[0]
  // clear R6 bits of word counter from prior communications to prevent dest++
  // on 1st port interrupt
  asm("CLR R6");

  P1IE = 0;
  P1IES &= ~RX_PIN; // Make positive edge for port interrupt to detect start of
                    // delimiter
  P1IFG = 0;  // Clear interrupt flag

  P1IE  |= RX_PIN; // Enable Port1 interrupt
  _BIS_SR(LPM4_bits | GIE);
  return;
}

inline void sleep()
{
  P1OUT &= ~RX_EN_PIN;

#if MONITOR_DEBUG_ON
  // for monitor - set SLEEP debug line - 01000 - 8
  P1OUT |= wisp_debug_1;
  P1OUT &= ~wisp_debug_1;
  P2OUT |= wisp_debug_2;
  P3OUT = 0x00;
#endif

  // enable port interrupt for voltage supervisor
  P2IES = 0;
  P2IFG = 0;
  P2IE |= VOLTAGE_SV_PIN;
  P1IE = 0;
  P1IFG = 0;
  TACTL = 0;

  _BIC_SR(GIE); // temporarily disable GIE so we can sleep and enable interrupts
                // at the same time
  P2IE |= VOLTAGE_SV_PIN; // Enable Port2 interrupt

  if (is_power_good())
    P2IFG = VOLTAGE_SV_PIN;

  _BIS_SR(LPM4_bits | GIE);

//  P1OUT |= RX_EN_PIN;
  return;
}

unsigned short is_power_good()
{
  return P2IN & VOLTAGE_SV_PIN;
}


//*************************************************************************
//************************ PORT 2 INTERRUPT *******************************

// Pin Setup :
// Description : Port 2 interrupt wakes on power good signal from supervisor.

#pragma vector=PORT2_VECTOR
__interrupt void Port2_ISR(void)   // (5-6 cycles) to enter interrupt
{
  P2IFG = 0x00;
  P2IE = 0;       // Interrupt disable
  P1IFG = 0;
  P1IE = 0;
  TACTL = 0;
  TACCTL0 = 0;
  TACCTL1 = 0;
  TAR = 0;
  state = STATE_READY;
  LPM4_EXIT;
}

#if USE_2132
#pragma vector=TIMER0_A0_VECTOR
#else
#pragma vector=TIMERA0_VECTOR
#endif
__interrupt void TimerA0_ISR(void)   // (5-6 cycles) to enter interrupt
{
  TACTL = 0;    // have to manually clear interrupt flag
  TACCTL0 = 0;  // have to manually clear interrupt flag
  TACCTL1 = 0;  // have to manually clear interrupt flag
  LPM4_EXIT;
}

//*************************************************************************
//************************ PORT 1 INTERRUPT *******************************

// warning   :  Whenever the clock frequency changes, the value of TAR should be
//              changed in aesterick lines
// Pin Setup :  P1.2
// Description : Port 1 interrupt is used as finding delimeter.

#pragma vector=PORT1_VECTOR
__interrupt void Port1_ISR(void)   // (5-6 cycles) to enter interrupt
{


#if USE_2132
  asm("MOV TA0R, R7");  // move TAR to R7(count) register (3 CYCLES)
#else
  asm("MOV TAR, R7");  // move TAR to R7(count) register (3 CYCLES)
#endif
  P1IFG = 0x00;       // 4 cycles
  TAR = 0;            // 4 cycles
  LPM4_EXIT;

  asm("CMP #0000h, R5\n"          // if (bits == 0) (1 cycle)
      "JEQ bit_Is_Zero_In_Port_Int\n"                // 2 cycles
  // bits != 0:
      "MOV #0000h, R5\n"          // bits = 0  (1 cycles)

      "CMP #0010h, R7\n"          // finding delimeter (12.5us, 2 cycles)
                                    // 2d -> 14
      "JNC delimiter_Value_Is_wrong\n"            //(2 cycles)
      "CMP #0040h, R7\n"            // finding delimeter (12.5us, 2 cycles)
                                    // 43H
      "JC  delimiter_Value_Is_wrong\n"
      "CLR P1IE\n"
#if USE_2132
      "BIS #8010h, TA0CCTL1\n"     // (5 cycles)   TACCTL1 |= CM1 + CCIE
#else
      "BIS #8010h, TACCTL1\n"     // (5 cycles)   TACCTL1 |= CM1 + CCIE
#endif
      "MOV #0004h, P1SEL\n"       // enable TimerA1    (4 cycles)
      "RETI\n"

      "delimiter_Value_Is_wrong:\n"
      "BIC #0004h, P1IES\n"
      "MOV #0000h, R5\n"          // bits = 0  (1 cycles)
      "MOV #0001h, &delimiterNotFound\n"
      "RETI\n"

      "bit_Is_Zero_In_Port_Int:\n"                 // bits == 0

#if USE_2132
      "MOV #0000h, TA0R\n"     // reset timer (4 cycles)
#else
      "MOV #0000h, TAR\n"     // reset timer (4 cycles)
#endif
      "BIS #0004h, P1IES\n"   // 4 cycles  change port interrupt edge to neg
      "INC R5\n"            // 1 cycle
      "RETI\n");

}
//*************************************************************************
//************************ Timer INTERRUPT *******************************

// Pin Setup :  P1.2
// Description :

#if USE_2132
#pragma vector=TIMER0_A1_VECTOR
#else
#pragma vector=TIMERA1_VECTOR
#endif
__interrupt void TimerA1_ISR(void)   // (6 cycles) to enter interrupt
{

    asm("MOV 0174h, R7");  // move TACCR1 to R7(count) register (3 CYCLES)
    TAR = 0;               // reset timer (4 cycles)
    TACCTL1 &= ~CCIFG;      // must manually clear interrupt flag (4 cycles)

    //<------up to here 26 cycles + 6 cyles of Interrupt == 32 cycles -------->
    asm("CMP #0003h, R5\n"      // if (bits >= 3).  it will do store bits
        "JGE bit_Is_Over_Three\n"
    // bit is not 3
        "CMP #0002h, R5\n"   // if ( bits == 2)
        "JEQ bit_Is_Two\n"         // if (bits == 2).

    // <----------------- bit is not 2 ------------------------------->
        "CMP #0001h, R5\n"      // if (bits == 1) -- measure RTcal value.
        "JEQ bit_Is_One\n"          // bits == 1

    // <-------------------- this is bit == 0 case --------------------->
        "bit_Is_Zero_In_Timer_Int:\n"
        "CLR R6\n"
        "INC R5\n"        // bits++
        "RETI\n"
    // <------------------- end of bit 0  --------------------------->

    // <-------------------- this is bit == 1 case --------------------->
        "bit_Is_One:\n"         // bits == 1.  calculate RTcal value
        "MOV R7, R9\n"       // 1 cycle
        "RRA R7\n"    // R7(count) is divided by 2.   1 cycle
        "MOV #0FFFFh, R8\n"   // R8(pivot) is set to max value    1 cycle
        "SUB R7, R8\n"        // R8(pivot) = R8(pivot) -R7(count/2) make new
                                // R8(pivot) value     1 cycle
        "INC R5\n"        // bits++
        "CLR R6\n"
        "RETI\n"
    // <------------------ end of bit 1 ------------------------------>

    // <-------------------- this is bit == 2 case --------------------->
        "bit_Is_Two:\n"
        "CMP R9, R7\n"    // if (count > (R9)(180)) this is hardcoded number,
                            // so have  to change to proper value
        "JGE this_Is_TRcal\n"
    // this is data
        "this_Is_Data_Bit:\n"
        "ADD R8, R7\n"   // count = count + pivot
    // store bit by shifting carry flag into cmd[bits]=(dest*) and increment
    // dest* (5 cycles)
        "ADDC.b @R4+,-1(R4)\n" // roll left (emulated by adding to itself ==
                                 // multiply by 2 + carry)
    // R6 lets us know when we have 8 bits, at which point we INC dest* (1
    // cycle)
        "INC R6\n"
        "CMP #0008,R6\n\n"   // undo increment of dest* (R4) until we have 8
                               // bits
        "JGE out_p\n"
        "DEC R4\n"
        "out_p:\n"           // decrement R4 if we haven't gotten 16 bits yet
                               // (3 or 4 cycles)
        "BIC #0008h,R6\n"   // when R6=8, this will set R6=0   (1 cycle)
        "INC R5\n"
        "RETI\n"
    // <------------------ end of bit 2 ------------------------------>

        "this_Is_TRcal:\n"
        "MOV R7, R5\n"    // bits = count. use bits(R5) to assign new value of
                            // TRcal
        "MOV R5, &TRcal\n"  // assign new value     (4 cycles)
        "MOV #0003h, R5\n"      // bits = 3..assign 3 to bits, so it will keep
                                  // track of current bits    (2 cycles)
        "CLR R6\n" // (1 cycle)
        "RETI\n"

   // <------------- this is bits >= 3 case ----------------------->
        "bit_Is_Over_Three:\n"     // bits >= 3 , so store bits
        "ADD R8, R7\n"    // R7(count) = R8(pivot) + R7(count),
    // store bit by shifting carry flag into cmd[bits]=(dest*) and increment
    // dest* (5 cycles)
        "ADDC.b @R4+,-1(R4)\n" // roll left (emulated by adding to itself ==
                                 // multiply by 2 + carry)
    // R6 lets us know when we have 8 bits, at which point we INC dest* (1
    // cycle)
        "INC R6\n"
        "CMP #0008,R6\n"   // undo increment of dest* (R4) until we have 8
                             // bits
        "JGE out_p1\n"
        "DEC R4\n"
        "out_p1:\n"        // decrement R4 if we haven't gotten 16 bits yet
                             // (3 or 4 cycles)
        "BIC #0008h,R6\n"  // when R6=8, this will set R6=0   (1 cycle)
        "INC R5\n"         // bits++
        "RETI");
    // <------------------ end of bit is over 3 ------------------------------>
}



//
//
// experimental M4 code
//
//

/******************************************************************************
*   Pin Set up
*   P1.1 - communication output
*******************************************************************************/
void sendToReader(volatile unsigned char *data, unsigned char numOfBits)
{

  SEND_CLOCK;

  TACTL &= ~TAIE;
  TAR = 0;
  // assign data address to dest
  dest = data;
  // Setup timer
  P1SEL |= TX_PIN; //  select TIMER_A0
//  P1DIR |= TX_PIN; // already set.
  TACTL |= TACLR;   //reset timer A
  TACTL = TASSEL1 + MC0;     // up mode

  TACCR0 = 5;  // this is 1 us period( 3 is 430x12x1)

  TAR = 0;
  TACCTL0 = OUTMOD2; // RESET MODE

#if MILLER_4_ENCODING
  BCSCTL2 |= DIVM_1;
#endif

  //TACTL |= TASSEL1 + MC1 + TAIE;
  //TACCTL1 |= SCS + CAP;   //initially, it set up as capturing rising edge.

/*******************************************************************************
*   The starting of the transmitting code. Transmitting code must send 4 or 16
*   of M/LF, then send 010111 preamble before sending data package. TRext
*   determines how many M/LFs are sent.
*
*   Used Register
*   R4 = CMD address, R5 = bits, R6 = counting 16 bits, R7 = 1 Word data, R9 =
*   temp value for loop R10 = temp value for the loop, R13 = 16 bits compare,
*   R14 = timer_value for 11, R15 = timer_value for 5
*******************************************************************************/


  //<-------- The below code will initiate some set up ---------------------->//
    //asm("MOV #05h, R14");
    //asm("MOV #02h, R15");
    bits = TRext;       // 6 cycles
    asm("CMP #0001h, R5\n"  // 1 cycles
        "JEQ TRextIs_1\n"   // 2 cycles
        "MOV #0004h, R9\n"   // 1 cycles
        "JMP otherSetup\n"   // 2 cycles

    // initialize loop for 16 M/LF
        "TRextIs_1:\n"
        "MOV #000fh, R9\n"    // 2 cycles    *** this will chagne to right value
        "NOP\n"

    //
        "otherSetup:");
    bits = numOfBits;                // (3 cycles).  This value will be
                                     // adjusted. if numOfBit is constant, it
                                     // takes 2 cycles
    asm("MOV #0bh, R14\n" // (2 cycles) R14 is used as timer value 11, it will
                          // be 2 us in 6 MHz
        "MOV #05h, R15\n" // (2 cycles) R15 is used as tiemr value 5, it will be
                          // 1 us in 6 MHz
        "MOV @R4+, R7\n"  // (2 cycles) Assign data to R7
        "MOV #0010h, R13\n"   // (2 cycles) Assign decimal 16 to R13, so it will
                              // reduce the 1 cycle from below code
        "MOV R13, R6\n"       // (1 cycle)
        "SWPB R7\n"           // (1 cycle)    Swap Hi-byte and Low byte
        "NOP\n"
        "NOP\n"
    // new timing needs 11 cycles
        "NOP\n"
    //asm("NOP\n"       // up to here, it make 1 to 0 transition.
    //<----------------1 us --------------------------------
    //    "NOP\n"   // 1
    //    "NOP\n"   // 2
    //    "NOP\n"   // 3
    //    "NOP\n"   // 4
    //    "NOP\n"   // 5
    //    "NOP\n"   // 6
    //    "NOP\n"   // 7
    //    "NOP\n"   // 8
    //    "NOP\n"   // 9
    // <---------- End of 1 us ------------------------------
    // The below code will create the number of M/LF.  According to the spec,
    // if the TRext is 0, there are 4 M/LF.  If the TRext is 1, there are 16
    // M/LF
    // The upper code executed 1 M/LF, so the count(R9) should be number of M/LF
    // - 1
    //asm("MOV #000fh, R9\n"    // 2 cycles  *** this will chagne to right value
        "MOV #0001h, R10\n"   // 1 cycles
    // The below code will create the number base encoding waveform., so the
    // number of count(R9) should be times of M
    // For example, if M = 2 and TRext are 1(16, the number of count should be
    // 32.
        "M_LF_Count:\n"
        "NOP\n"   // 1
        "NOP\n"   // 2
        "NOP\n"   // 3
        "NOP\n"   // 4
        "NOP\n"   // 5
        "NOP\n"   // 6
        "NOP\n"   // 7
        "NOP\n"   // 8
        "NOP\n"   // 9
        "NOP\n"   // 10
        "NOP\n"   // 11
        "NOP\n"   // 12
        "NOP\n"   // 13
        "NOP\n"   // 14
        "NOP\n"   // 15
        "NOP\n"   // 16
        // "NOP\n"   // 17

        "CMP R10, R9\n"       // 1 cycle
        "JEQ M_LF_Count_End\n" // 2 cycles
        "INC R10\n"           // 1 cycle
        "NOP\n"   // 22
        "JMP M_LF_Count\n"      // 2 cycles

        "M_LF_Count_End:\n"
    // this code is preamble for 010111 , but for the loop, it will only send
    // 01011
        "MOV #5c00h, R9\n"      // 2 cycles
        "MOV #0006h, R10\n"     // 2 cycles
    // this should be counted as 0. Therefore, Assembly DEC line should be 1
    // after executing
        "Preamble_Loop:\n"
        "DEC R10\n"               // 1 cycle
        "JZ last_preamble_set\n"          // 2 cycle
        "RLC R9\n"                // 1 cycle
        "JNC preamble_Zero\n"     // 2 cycle      .. up to 6
    // this is 1 case for preamble
        "NOP\n"
#if USE_2132
        "MOV R14, TA0CCR0\n"       // 4 cycle      .. 10
#else
        "MOV R14, TACCR0\n"       // 4 cycle      .. 10
#endif
        "NOP\n"
        "NOP\n"
        "NOP\n"
#if USE_2132
        "MOV R15, TA0CCR0\n"       // 4 cycle      .. 19
#else
        "MOV R15, TACCR0\n"       // 4 cycle      .. 19
#endif
        "NOP\n"
        "NOP\n"
        "NOP\n"
        "NOP\n"                   // .. 22
        "JMP Preamble_Loop\n"     // 2 cycles   .. 24

    // this is 0 case for preamble
        "preamble_Zero:\n"
        "NOP\n"
        "NOP\n"
        "NOP\n"
        "NOP\n"
        "NOP\n"
        "NOP\n"
        "NOP\n"
        "NOP\n"
        "NOP\n"
        "NOP\n"
        "NOP\n"
        "NOP\n"
        "NOP\n"
        "NOP\n"
        "NOP\n"
        "NOP\n"


        "JMP Preamble_Loop\n"     // 2 cycles .. 24

        "last_preamble_set:\n"
        "NOP\n"         // 4
        "NOP\n"
        "NOP\n"    // TURN ON
        "NOP\n"
#if USE_2132
        "MOV.B R14, TA0CCR0\n"// 4 cycles
#else
        "MOV.B R14, TACCR0\n"// 4 cycles
#endif
        "NOP\n"
        "NOP\n"
        "NOP\n"
#if USE_2132
        "MOV.B R15, TA0CCR0\n"
#else
        "MOV.B R15, TACCR0\n"
#endif
        "NOP\n"
        "NOP\n"
        "NOP\n"
        "NOP\n"
        "NOP\n"
    //asm("NOP");
    //<------------- end of initial set up

/***********************************************************************
*   The main loop code for transmitting data in 6 MHz.  This will transmit data
*   in real time.
*   R5(bits) and R6(word count) must be 1 bigger than desired value.
*   Ex) if you want to send 16 bits, you have to store 17 to R5.
************************************************************************/

    // this is starting of loop
        "LOOPAGAIN:\n"
        "DEC R5\n"                              // 1 cycle
        "JEQ Three_Cycle_Loop_End\n"                // 2 cycle
    //<--------------loop condition ------------
        "NOP\n"                                 // 1 cycle
        "RLC R7\n"                              // 1 cycle
        "JNC bit_is_zero\n"                 // 2 cycles  ..7

    // bit is 1
        "bit_is_one:\n"
#if USE_2132
        "MOV R14, TA0CCR0\n"                   // 4 cycles   ..11
#else
        "MOV R14, TACCR0\n"                   // 4 cycles   ..11
#endif                // 4 cycles   ..11
        "DEC R6\n"                              // 1 cycle  ..12
        "JNZ bit_Count_Is_Not_16\n"              // 2 cycle    .. 14
    // This code will assign new data from reply and then swap bytes.  After
    // that, update R6 with 16 bits
    //asm("MOV @R4+, R7");
#if USE_2132
        "MOV R15, TA0CCR0\n"                   // 4 cycles   .. 20
#else
        "MOV R15, TACCR0\n"                   // 4 cycles   .. 20
#endif
        "MOV R13, R6\n"                         // 1 cycle    .. 22
    //asm("MOV R15, TACCR0");                   // 4 cycles   .. 20
        "MOV @R4+, R7\n"

        "SWPB R7\n"                             // 1 cycle    .. 21
    //asm("MOV R13, R6");                         // 1 cycle    .. 22
    // End of assigning data byte
        "JMP LOOPAGAIN\n"                       // 2 cycle    .. 24

        "seq_zero:\n"
        "NOP\n"                         // 1 cycle   .. 3
#if USE_2132
        "MOV R15, TA0CCR0\n"         // 4 cycles       ..7
#else
        "MOV R15, TACCR0\n"         // 4 cycles       ..7
#endif

    // bit is 0, so it will check that next bit is 0 or not
        "bit_is_zero:\n"                // up to 7 cycles
        "DEC R6\n"                      // 1 cycle   .. 8
        "JNE bit_Count_Is_Not_16_From0\n"           // 2 cycles  .. 10
    // bit count is 16
        "DEC R5\n"                      // 1 cycle   .. 11
        "JEQ Thirteen_Cycle_Loop_End\n"     // 2 cycle   .. 13
    // This code will assign new data from reply and then swap bytes.  After
    // that, update R6 with 16 bits
        "MOV @R4+,R7\n"                 // 2 cycles     15
        "SWPB R7\n"                     // 1 cycle      16
        "MOV R13, R6\n"                 // 1 cycles     17
    // End of assigning new data byte
        "RLC R7\n"              // 1 cycles     18
        "JC nextBitIs1\n"           // 2 cycles  .. 20
    // bit is 0
#if USE_2132
        "MOV R14, TA0CCR0\n"             // 4 cycles  .. 24
#else
        "MOV R14, TACCR0\n"             // 4 cycles  .. 24
#endif
    // Next bit is 0 , it is 00 case
        "JMP seq_zero\n"

// <---------this code is 00 case with no 16 bits.
        "bit_Count_Is_Not_16_From0:\n"                  // up to 10 cycles
        "DEC R5\n"                          // 1 cycle      11
        "JEQ Thirteen_Cycle_Loop_End\n"         // 2 cycle    ..13
        "NOP\n"                         // 1 cycles    ..14
        "NOP\n"                             // 1 cycles    ..15
        "NOP\n"                             // 1 cycles    ..16
        "NOP\n"                             // 1 cycles    ..17
        "RLC R7\n"                      // 1 cycle     .. 18
        "JC nextBitIs1\n"               // 2 cycles    ..20
#if USE_2132
        "MOV R14, TA0CCR0\n"               // 4 cycles   .. 24
#else
        "MOV R14, TACCR0\n"               // 4 cycles   .. 24
#endif
        "JMP seq_zero\n"        // 2 cycles    .. 2

// whenever current bit is 0, then next bit is 1
        "nextBitIs1:\n"     // 20
        "NOP\n"
        "NOP\n"
        "NOP\n"
        "NOP\n"       // 24

        "NOP\n"
        "NOP\n"
        "NOP\n"
        "NOP\n"
        "NOP\n"
        "JMP bit_is_one\n"  // end of bit 0 .. 7

        "bit_Count_Is_Not_16:\n"       // up to here 14
        "NOP\n"
#if USE_2132
        "MOV R15, TA0CCR0\n"             // 4 cycles   .. 20
#else
        "MOV R15, TACCR0\n"             // 4 cycles   .. 20
#endif
        "NOP\n"
        "NOP\n"
        "NOP\n"
        "JMP LOOPAGAIN\n"     // 2 cycle          .. 24

    // below code is the end of loop code
        "Three_Cycle_Loop_End:\n"
        "JMP lastBit\n"     // 2 cycles   .. 5

        "Thirteen_Cycle_Loop_End:\n"
        "NOP\n"   // 1
        "NOP\n"   // 2
        "NOP\n"   // 3
        "NOP\n"   // 4
        "NOP\n"   // 5
        "NOP\n"   // 6
        "NOP\n"   // 7
        "NOP\n"   // 8
        "NOP\n"   // 9
        "NOP\n"   // 10
        "NOP\n"   // 11 ..24
        "NOP\n"   // 12
        "NOP\n"   // 13
        "NOP\n"   // 14
        "JMP lastBit\n"
/***********************************************************************
*   End of main loop
************************************************************************/
// this is last data 1 bit which is dummy data
        "lastBit:\n"
        "NOP\n"
        "NOP\n"
#if USE_2132
        "MOV.B R14, TA0CCR0\n"// 4 cycles
#else
        "MOV.B R14, TACCR0\n"// 4 cycles
#endif
        "NOP\n"
        "NOP\n"
        "NOP\n"
#if USE_2132
        "MOV.B R15, TA0CCR0\n"
#else
        "MOV.B R15, TACCR0\n"
#endif
        "NOP\n"
        "NOP\n"
    // experiment

        "NOP");

    //TACCR0 = 0;

    TACCTL0 = 0;  // DON'T NEED THIS NOP
    RECEIVE_CLOCK;

}




/**
 * This code comes from the Open Tag Systems Protocol Reference Guide version
 * 1.1 dated 3/23/2004.
 * (http://www.opentagsystems.com/pdfs/downloads/OTS_Protocol_v11.pdf)
 * No licensing information accompanied the code snippet.
 **/
unsigned short crc16_ccitt(volatile unsigned char *data, unsigned short n) {
  register unsigned short i, j;
  register unsigned short crc_16;

  crc_16 = 0xFFFF; // Equivalent Preset to 0x1D0F
  for (i=0; i<n; i++) {
    crc_16^=data[i] << 8;
    for (j=0;j<8;j++) {
      if (crc_16&0x8000) {
        crc_16 <<= 1;
        crc_16 ^= 0x1021; // (CCITT) x16 + x12 + x5 + 1
      }
      else {
        crc_16 <<= 1;
      }
    }
  }
  return(crc_16^0xffff);
}

void crc16_ccitt_readReply(unsigned int numDataBytes)
{

  // shift everything over by 1 to accomodate leading "0" bit.
  // first, grab address of beginning of array
  readReply[numDataBytes + 2] = 0; // clear out this spot for the loner bit of
                                   // handle
  readReply[numDataBytes + 4] = 0; // clear out this spot for the loner bit of
                                   // crc
  bits = (unsigned short) &readReply[0];
  // shift all bytes and later use only data + handle
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  asm("RRC.b @R5+");
  // store loner bit in array[numDataBytes+2] position
  asm("RRC.b @R5+");
  // make first bit 0
  readReply[0] &= 0x7f;

  // compute crc on data + handle bytes
  readReplyCRC = crc16_ccitt(&readReply[0], numDataBytes + 2);
  readReply[numDataBytes + 4] = readReply[numDataBytes + 2];
  // XOR the MSB of CRC with loner bit.
  readReply[numDataBytes + 4] ^= __swap_bytes(readReplyCRC); // XOR happens with
                                                             // MSB of lower
                                                             // nibble
  // Just take the resulting bit, not the whole byte
  readReply[numDataBytes + 4] &= 0x80;

  unsigned short mask = __swap_bytes(readReply[numDataBytes + 4]);
  mask >>= 3;
  mask |= (mask >> 7);
  mask ^= 0x1020;
  mask >>= 1;  // this is because the loner bit pushes the CRC to the left by 1
  // but we don't shift the crc because it should get pushed out by 1 anyway
  readReplyCRC ^= mask;

  readReply[numDataBytes + 3] = (unsigned char) readReplyCRC;
  readReply[numDataBytes + 2] |= (unsigned char) (__swap_bytes(readReplyCRC) &
          0x7F);
}

#if 0
// not used now, but will need for later
unsigned char crc5(volatile unsigned char *buf, unsigned short numOfBits)
{
  register unsigned char shift;
  register unsigned char data, val;
  register unsigned short i;
  shift = 0x48;
  for (i = 0; i < numOfBits; i++)
  {
    if ( (i%8) == 0)
      data = *buf++;
    val = shift ^ data;
    shift = shift << 1;
    data = data << 1;
    if (val&0x80)
      shift = shift ^ POLY5;
  }
  shift = shift >>3;
  return (unsigned char)(shift);
}
#endif

#if ENABLE_SLOTS

void lfsr()
{
    // calculate LFSR
    rn16 = (rn16 << 1) | (((rn16 >> 15) ^ (rn16 >> 13) ^
                (rn16 >> 9) ^ (rn16 >> 8)) & 1);
    rn16 = rn16 & 0xFFFF;

    // fit 2^Q-1
    rn16 = rn16>>(15-Q);
}

inline void loadRN16()
{
#if 1
  if (Q > 8)
  {
    queryReply[0] = RN16[(Q<<1)-9];
    queryReply[1] = RN16[(Q<<1)-8];
  }
  else
  {
    int index = ((Q+shift) & 0xF);
    queryReply[0] = RN16[index];
    queryReply[1] = RN16[index+1];
  }
#else
    queryReply[0] = 0xf0;
    queryReply[1] = 0x0f;
#endif

}

inline void mixupRN16()
{
  unsigned short tmp;
  unsigned short newQ = 0;
  unsigned short swapee_index = 0;

  newQ = RN16[shift] & 0xF;
  swapee_index = RN16[newQ];
  tmp = RN16[shift];
  RN16[Q] = RN16[swapee_index];
  RN16[swapee_index] = tmp;
}


#endif

#if ENABLE_SESSIONS
// initialize sessions for power-on
void initialize_sessions()
{

    SL = SL_NOT_ASSERTED;   // selected flag powers up deasserted
    // all inventory flags power up in state 'A'
    session_table[S0_INDEX] = SESSION_STATE_A;
        session_table[S1_INDEX] = SESSION_STATE_A;
        session_table[S2_INDEX] = SESSION_STATE_A;
        session_table[S3_INDEX] = SESSION_STATE_A;
}
void handle_session_timeout()
{
#if 0
    // selected flag is persistent throughout power up state
    // S0 inventory flag is persistent throughout power up state
    // S1 is reset, unless it is in the middle of an inventory round
    if ( ! inInventoryRound ) session_table[S1_INDEX] = SESSION_STATE_A;
    // S2 and S3 are always refreshed
    session_table[S2_INDEX] = SESSION_STATE_A;
    session_table[S3_INDEX] = SESSION_STATE_A;
#endif
}
#endif

#if ENABLE_SESSIONS
// compare two chunks of memory, starting at given bit offsets (relative to the
// starting byte, that is).
// startingBitX is a range from 7 (MSbit) to 0 (LSbit). Len is number of bits.
// Returns a // 1 if they match and a 0 if they don't match.
inline int bitCompare(unsigned char *startingByte1,
        unsigned short startingBit1,
        unsigned char *startingByte2,
        unsigned short startingBit2,
        unsigned short len)
{
    unsigned char test1, test2;

    while ( len-- ) {

        test1 = (*startingByte1) & ( 1 << startingBit1 );
        test2 = (*startingByte2) & ( 1 << startingBit2 );

        if ( (test1 == 0 && test2 != 0) || ( test1 != 0 && test2 == 0 ) ) {
            return 0;
        }

        if ( len == 0 ) return 1;

        if ( startingBit1 == 0 ) {
            startingByte1++;
            startingBit1 = 7;
        }
        else
            startingBit1--;

        if ( startingBit2 == 0 ) {
            startingByte2++;
            startingBit2 = 7;
        }
        else
            startingBit2--;

    }

        return 1;
}
#endif
