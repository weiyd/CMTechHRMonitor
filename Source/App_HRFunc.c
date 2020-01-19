
#include "App_HRFunc.h"
#include "hal_types.h"

// Time interval constants.

#define MS80	16
#define MS95	19
#define MS150	30
#define MS200	40
#define MS360	72
#define MS450	90
#define MS1000	200
#define MS1500	300

#define WINDOW_WIDTH	MS80
#define FILTER_DELAY	21 + MS200

// Global values for QRS detector.

static int Q0 = 0, Q1 = 0, Q2 = 0, Q3 = 0, Q4 = 0, Q5 = 0, Q6 = 0, Q7 = 0 ;
static int N0 = 0, N1 = 0, N2 = 0, N3 = 0, N4 = 0, N5 = 0, N6 = 0, N7 = 0 ;
static int RR0=0, RR1=0, RR2=0, RR3=0, RR4=0, RR5=0, RR6=0, RR7=0 ;
static int QSum = 0, NSum = 0, RRSum = 0 ;
static int det_thresh, sbcount ;
static int tempQSum, tempNSum, tempRRSum ;
static int QN0=0, QN1=0 ;
static int Reg0=0 ;

static void UpdateQ(int16 newQ);
static void UpdateN(int16 newN);
static void UpdateRR(int16 newRR);
static int16 lpfilt( int16 datum ,int init);
static int16 hpfilt( int16 datum, int init );
static int16 deriv1( int16 x0, int init );
static int16 mvwint( int16 datum, int init);
static int16 Peak( int16 datum, int init );

/******************************************************************************
*  PICQRSDet takes 16-bit ECG samples (5 uV/LSB) as input and returns the
*  detection delay when a QRS is detected.  Passing a nonzero value for init
*  resets the QRS detector.
******************************************************************************/
 
int16 PICQRSDet(int16 x, int init)
{
  static int16 tempPeak, initMax ;
  static uint8 preBlankCnt=0, qpkcnt=0, initBlank=0 ;
  static int16 count, sbpeak, sbloc ;
  int16 QrsDelay = 0 ;
  int16 temp0, temp1 ;
  
  if(init)
  {		
    hpfilt(0,1) ;
    lpfilt(0,1) ;
    deriv1(0,1) ;
    mvwint(0,1) ;
    Peak(0,1) ;
    qpkcnt = count = sbpeak = 0 ;
    QSum = NSum = 0 ;
  
    RRSum = MS1000<<3 ;
    RR0=RR1=RR2=RR3=RR4=RR5=RR6=RR7=MS1000 ;
  
    Q0=Q1=Q2=Q3=Q4=Q5=Q6=Q7=0 ;		
    N0=N1=N2=N3=N4=N5=N6=N7=0 ;
    NSum = 0 ;
  
    return(0) ;
  }
  
  x = lpfilt(x,0) ;
  x = hpfilt(x,0) ;
  x = deriv1(x,0) ;
  if(x < 0) x = -x ;
  x = mvwint(x,0) ;
  x = Peak(x,0) ;
  
  
  // Hold any peak that is detected for 200 ms
  // in case a bigger one comes along.  There
  // can only be one QRS complex in any 200 ms window.
  
  if(!x && !preBlankCnt)
    x = 0 ;
  
  else if(!x && preBlankCnt)	// If we have held onto a peak for
  {				// 200 ms pass it on for evaluation.
    if(--preBlankCnt == 0)
      x = tempPeak ;
    else 
      x = 0 ;
  }
  
  else if(x && !preBlankCnt)		// If there has been no peak for 200 ms
  {			// save this one and start counting.
    tempPeak = x ;
    preBlankCnt = MS200 ;
    x = 0 ;
  }
  
  else if(x)				// If we were holding a peak, but
  {				// this ones bigger, save it and
  if(x > tempPeak)		// start counting to 200 ms again.
    {
      tempPeak = x ;
      preBlankCnt = MS200 ;
      x = 0 ;
    }
  else if(--preBlankCnt == 0)
    x = tempPeak ;
  else x = 0 ;
  }
  
  // Initialize the qrs peak buffer with the first eight
  // local maximum peaks detected.
  
  if( qpkcnt < 8 )
  {
    ++count ;
    if(x > 0) count = WINDOW_WIDTH ;
    if(++initBlank == MS1000)
    {
      initBlank = 0 ;
      UpdateQ(initMax) ;
      initMax = 0 ;
      ++qpkcnt ;
      if(qpkcnt == 8)
      {
        RRSum = MS1000<<3 ;
        RR0=RR1=RR2=RR3=RR4=RR5=RR6=RR7=MS1000 ;

        sbcount = MS1500+MS150 ;
      }
    }
    if( x > initMax )
      initMax = x ;
  }
  
  else
  {
    ++count ;
    
    // Check if peak is above detection threshold.
  
    if(x > det_thresh)
    {
      UpdateQ(x) ;
  
      // Update RR Interval estimate and search back limit
  
      UpdateRR(count-WINDOW_WIDTH) ;
      count=WINDOW_WIDTH ;
      sbpeak = 0 ;
      QrsDelay = WINDOW_WIDTH+FILTER_DELAY ;
    }
    
    // If a peak is below the detection threshold.
  
    else if(x != 0)
    {
      UpdateN(x) ;
  
      QN1=QN0 ;
      QN0=count ;
  
      if((x > sbpeak) && ((count-WINDOW_WIDTH) >= MS360))
      {
        sbpeak = x ;
        sbloc = count-WINDOW_WIDTH ;
      }

    }
            
    // Test for search back condition.  If a QRS is found in
    // search back update the QRS buffer and det_thresh.
  
    if((count > sbcount) && (sbpeak > (det_thresh >> 1)))
    {
      UpdateQ(sbpeak) ;
  
      // Update RR Interval estimate and search back limit
  
      UpdateRR(sbloc) ;
  
      QrsDelay = count = count - sbloc;
      QrsDelay += FILTER_DELAY ;
      sbpeak = 0 ;
    }
  }
                                  
  return(QrsDelay) ;
}
	

/**************************************************************************
*  UpdateQ takes a new QRS peak value and updates the QRS mean estimate
*  and detection threshold.
**************************************************************************/

static void UpdateQ(int16 newQ)
{
  QSum -= Q7 ;
  Q7=Q6; Q6=Q5; Q5=Q4; Q4=Q3; Q3=Q2; Q2=Q1; Q1=Q0;
  Q0=newQ ;
  QSum += Q0 ;
  
  det_thresh = QSum-NSum ;
  det_thresh = NSum + (det_thresh>>1) - (det_thresh>>3) ;
  
  det_thresh >>= 3 ;
}


/**************************************************************************
*  UpdateN takes a new noise peak value and updates the noise mean estimate
*  and detection threshold.
**************************************************************************/

static void UpdateN(int16 newN)
{
  NSum -= N7 ;
  N7=N6; N6=N5; N5=N4; N4=N3; N3=N2; N2=N1; N1=N0; N0=newN ;
  NSum += N0 ;
  
  det_thresh = QSum-NSum ;
  det_thresh = NSum + (det_thresh>>1) - (det_thresh>>3) ;
  
  det_thresh >>= 3 ;
}

/**************************************************************************
*  UpdateRR takes a new RR value and updates the RR mean estimate
**************************************************************************/

static void UpdateRR(int16 newRR)
{
  RRSum -= RR7 ;
  RR7=RR6; RR6=RR5; RR5=RR4; RR4=RR3; RR3=RR2; RR2=RR1; RR1=RR0 ;
  RR0=newRR ;
  RRSum += RR0 ;
  
  sbcount=RRSum+(RRSum>>1) ;
  sbcount >>= 3 ;
  sbcount += WINDOW_WIDTH ;
}
	

/*************************************************************************
*  lpfilt() implements the digital filter represented by the difference
*  equation:
*
* 	y[n] = 2*y[n-1] - y[n-2] + x[n] - 2*x[n-5] + x[n-10]
*
*	Note that the filter delay is five samples.
*
**************************************************************************/

static int16 lpfilt( int16 datum ,int init)
{
  static int16 y1 = 0, y2 = 0 ;
  static int16 d0,d1,d2,d3,d4,d5,d6,d7,d8,d9 ;
  int16 y0 ;
  int16 output ;
  
  if(init)
  {
    d0=d1=d2=d3=d4=d5=d6=d7=d8=d9=0 ;
    y1 = y2 = 0 ;
  }
  
  y0 = (y1 << 1) - y2 + datum - (d4<<1) + d9 ;
  y2 = y1;
  y1 = y0;
  if(y0 >= 0) output = y0 >> 5;
  else output = (y0 >> 5) | 0xF800 ;
  
  d9=d8 ;
  d8=d7 ;
  d7=d6 ;
  d6=d5 ;
  d5=d4 ;
  d4=d3 ;
  d3=d2 ;
  d2=d1 ;
  d1=d0 ;
  d0=datum ;
                                          
  return(output) ;
}

/******************************************************************************
*  hpfilt() implements the high pass filter represented by the following
*  difference equation:
*
*	y[n] = y[n-1] + x[n] - x[n-32]
*	z[n] = x[n-16] - y[n] ;
*
*  Note that the filter delay is 15.5 samples
******************************************************************************/

#define HPBUFFER_LGTH	32

static int16 hpfilt( int16 datum, int init )
{
  static int16 y=0 ;
  static int16 data[HPBUFFER_LGTH] ;
  static int ptr = 0 ;
  int16 z ;
  int halfPtr ;
  
  if(init)
  {
    for(ptr = 0; ptr < HPBUFFER_LGTH; ++ptr)
            data[ptr] = 0 ;
    ptr = 0 ;
    y = 0 ;
    return(0) ;
  }
  
  y += datum - data[ptr];
  
  halfPtr = ptr-(HPBUFFER_LGTH/2) ;
  halfPtr &= 0x1F ;
  
  
  z = data[halfPtr] ;		// Compensate for CCS shift bug.
  if(y >= 0) z -= (y>>5) ;
  else z -= (y>>5)|0xF800 ;
  
  
  data[ptr] = datum ;
  ptr = (ptr+1) & 0x1F ;
  
  return( z );
}

/*****************************************************************************
*  deriv1 and deriv2 implement derivative approximations represented by
*  the difference equation:
*
*	y[n] = 2*x[n] + x[n-1] - x[n-3] - 2*x[n-4]
*
*  The filter has a delay of 2.
*****************************************************************************/

static int16 deriv1( int16 x0, int init )
{
  static int16 x1, x2, x3, x4 ;
  int16 output;
  if(init)
    x1 = x2 = x3 = x4 = 0 ;
  
  output = x1-x3 ;
  if(output < 0) output = (output>>1) | 0x8000 ;	// Compensate for shift bug.
  else output >>= 1 ;
  
  output += (x0-x4) ;
  if(output < 0) output = (output>>1) | 0x8000 ;
  else output >>= 1 ;
    
  x4 = x3 ;
  x3 = x2 ;
  x2 = x1 ;
  x1 = x0 ;
  return(output);
}


/*****************************************************************************
* mvwint() implements a moving window integrator, averaging
* the signal values over the last 16
******************************************************************************/

static int16 mvwint(int16 datum, int init)
{
  static uint16 sum = 0 ;
  static uint16 d0,d1,d2,d3,d4,d5,d6,d7,d8,d9,d10,d11,d12,d13,d14,d15 ;
  
  if(init)
  {
    d0=d1=d2=d3=d4=d5=d6=d7=d8=d9=d10=d11=d12=d13=d14=d15=0 ;
    sum = 0 ;
  }
  sum -= d15 ;
  
  d15=d14 ;
  d14=d13 ;
  d13=d12 ;
  d12=d11 ;
  d11=d10 ;
  d10=d9 ;
  d9=d8 ;
  d8=d7 ;
  d7=d6 ;
  d6=d5 ;
  d5=d4 ;
  d4=d3 ;
  d3=d2 ;
  d2=d1 ;
  d1=d0 ;
  if(datum >= 0x0400) d0 = 0x03ff ;
  else d0 = (datum>>2) ;
  sum += d0 ;
  
  return(sum>>2) ;
}


/**************************************************************
* peak() takes a datum as input and returns a peak height
* when the signal returns to half its peak height, or it has been
* 95 ms since the peak height was detected. 
**************************************************************/


static int16 Peak( int16 datum, int init )
{
  static int16 max = 0, lastDatum ;
  static int timeSinceMax = 0 ;
  int16 pk = 0 ;
  
  if(init)
  {
    max = 0 ;
    timeSinceMax = 0 ;
    return(0) ;
  }
          
  if(timeSinceMax > 0)
    ++timeSinceMax ;
  
  if((datum > lastDatum) && (datum > max))
  {
    max = datum ;
    if(max > 2)
      timeSinceMax = 1 ;
  }
  
  else if(datum < (max >> 1))
  {
    pk = max ;
    max = 0 ;
    timeSinceMax = 0 ;
  }
  
  else if(timeSinceMax > MS95)
  {
    pk = max ;
    max = 0 ;
    timeSinceMax = 0 ;
  }
  lastDatum = datum ;
  return(pk) ;
}