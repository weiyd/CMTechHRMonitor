/**************************************************************************************************
* CMTechHRMonitor.c: main application source file
**************************************************************************************************/

/*********************************************************************
 * INCLUDES
 */

#include "bcomdef.h"
#include "OSAL.h"
#include "OSAL_PwrMgr.h"
#include "osal_snv.h"
#include "linkdb.h"
#include "OnBoard.h"
#include "gatt.h"
#include "hci.h"
#include "gapgattserver.h"
#include "gattservapp.h"

#if defined ( PLUS_BROADCASTER )
  #include "peripheralBroadcaster.h"
#else
  #include "peripheral.h"
#endif
#include "gapbondmgr.h"
#include "CMTechHRMonitor.h"
#if defined FEATURE_OAD
  #include "oad.h"
  #include "oad_target.h"
#endif


#include "Service_DevInfo.h"
#include "Service_HRMonitor.h"
#include "service_battery.h"
#include "service_ecg.h"
#include "App_HRFunc.h"
#include "Dev_ADS1x9x.H"
#include "CMUtil.h"

#define ADVERTISING_INTERVAL 320 // ad interval, units of 0.625ms
#define ADVERTISING_DURATION 2000 // ad duration, units of ms
#define ADVERTISING_OFFTIME 8000 // ad offtime to wait for a next ad, units of ms

#define NVID_WORK_MODE 0x80      // the NVID of the work mode
#define MODE_HR 0x00    // HR work mode
#define MODE_ECG 0x01   // ECG work mode

// connection parameter in HR mode
#define HR_MODE_MIN_INTERVAL  302// 1584 //782//1580  , unit: 1.25ms
#define HR_MODE_MAX_INTERVAL  319// 1600 //799//1598  , unit: 1.25ms
#define HR_MODE_SLAVE_LATENCY  4// 2 //1//0
#define HR_MODE_CONNECT_TIMEOUT 600 // unit: 10ms, If no connection event occurred during this timeout, the connect will be shut down.

// connection parameter in ECG mode
#define ECG_MODE_MIN_INTERVAL 16  // unit: 1.25ms
#define ECG_MODE_MAX_INTERVAL 32  // unit: 1.25ms
#define ECG_MODE_SLAVE_LATENCY 4
#define ECG_MODE_CONNECT_TIMEOUT 100 // unit: 10ms, If no connection event occurred during this timeout, the connect will be shut down.

#define CONN_PAUSE_PERIPHERAL 4  // the pause time from the connection establishment to the update of the connection parameters

#define INVALID_CONNHANDLE 0xFFFF // invalid connection handle
#define STATUS_ECG_STOP 0x00     // ecg sampling stopped status
#define STATUS_ECG_START 0x01    // ecg sampling started status

#define HR_NOTI_PERIOD 2000 // heart rate notification period, ms
#define BATT_NOTI_PERIOD 120000L // battery notification period, ms
#define ECG_1MV_CALI_VALUE  160  //164  // ecg 1mV calibration value

static uint8 taskID;   
static uint16 gapConnHandle = INVALID_CONNHANDLE;
static gaprole_States_t gapProfileState = GAPROLE_INIT;
static uint8 attDeviceName[GAP_DEVICE_NAME_LEN] = "KM HRM"; // GGS device name
static uint8 status = STATUS_ECG_STOP; // ecg sampling status

uint16 SAMPLERATE; // ecg sample rate

// advertise data
static uint8 advertData[] = 
{ 
  0x02,   // length of this data
  GAP_ADTYPE_FLAGS,
  GAP_ADTYPE_FLAGS_GENERAL | GAP_ADTYPE_FLAGS_BREDR_NOT_SUPPORTED,

  // service UUID
  0x03,   // length of this data
  GAP_ADTYPE_16BIT_MORE,
  LO_UINT16( HRM_SERV_UUID ),
  HI_UINT16( HRM_SERV_UUID ),

};

// scan response data
static uint8 scanResponseData[] =
{
  0x05,   // length of this data
  GAP_ADTYPE_LOCAL_NAME_SHORT,   
  'K',
  'M',
  'H',
  'R'
};

static void gapStateCB( gaprole_States_t newState ); // gap state callback function
static void hrServiceCB( uint8 event ); // heart rate service callback function
static void battServiceCB( uint8 event ); // battery service callback function
static void ecgServiceCB( uint8 event ); // ecg service callback function

// GAP Role callback struct
static gapRolesCBs_t gapStateCBs =
{
  gapStateCB,         // Profile State Change Callbacks
  NULL                // When a valid RSSI is read from controller (not used by application)
};

static gapBondCBs_t bondCBs =
{
  NULL,                   // Passcode callback
  NULL                    // Pairing state callback
};

// Heart rate monitor service callback struct
static HRMServiceCBs_t hrServCBs =
{
  hrServiceCB   
};

// battery service callback struct
static BattServiceCBs_t battServCBs =
{
  battServiceCB    
};

// Ecg service callback struct
static ECGServiceCBs_t ecgServCBs =
{
  ecgServiceCB    
};

static void processOSALMsg( osal_event_hdr_t *pMsg ); // OSAL message process function
static void initIOPin(); // initialize IO pins
static void startEcgSampling( void ); // start ecg sampling
static void stopEcgSampling( void ); // stop ecg sampling
static void setParameter(uint8 mode);

extern void HRM_Init( uint8 task_id )
{ 
  taskID = task_id;
  uint8 mode;
  
  HCI_EXT_SetTxPowerCmd (LL_EXT_TX_POWER_0_DBM);
  
  // Setup the GAP Peripheral Role Profile
  {
    // set the advertising data and scan response data
    GAPRole_SetParameter( GAPROLE_ADVERT_DATA, sizeof( advertData ), advertData );
    GAPRole_SetParameter( GAPROLE_SCAN_RSP_DATA, sizeof ( scanResponseData ), scanResponseData );
    
    // set the advertising parameters
    GAP_SetParamValue( TGAP_GEN_DISC_ADV_INT_MIN, ADVERTISING_INTERVAL ); // units of 0.625ms
    GAP_SetParamValue( TGAP_GEN_DISC_ADV_INT_MAX, ADVERTISING_INTERVAL ); // units of 0.625ms
    GAP_SetParamValue( TGAP_GEN_DISC_ADV_MIN, ADVERTISING_DURATION ); // advertising duration
    uint16 gapRole_AdvertOffTime = ADVERTISING_OFFTIME;
    GAPRole_SetParameter( GAPROLE_ADVERT_OFF_TIME, sizeof( uint16 ), &gapRole_AdvertOffTime );
    
    // enable advertising
    uint8 advertising = TRUE;
    GAPRole_SetParameter( GAPROLE_ADVERT_ENABLED, sizeof( uint8 ), &advertising );

    GAP_SetParamValue( TGAP_CONN_PAUSE_PERIPHERAL, CONN_PAUSE_PERIPHERAL ); 
    
    // read ecg lock status from NV
    uint8 rtn = osal_snv_read(NVID_WORK_MODE, sizeof(uint8), (uint8*)&mode);
    if(rtn != SUCCESS)
      mode = MODE_HR;   
    
    setParameter(mode);
    
    uint8 enable_update_request = TRUE;
    GAPRole_SetParameter( GAPROLE_PARAM_UPDATE_ENABLE, sizeof( uint8 ), &enable_update_request );
  }
  
  // set GGS device name
  GGS_SetParameter( GGS_DEVICE_NAME_ATT, GAP_DEVICE_NAME_LEN, attDeviceName );

  // Setup the GAP Bond Manager
  {
    uint32 passkey = 0; // passkey "000000"
    uint8 pairMode = GAPBOND_PAIRING_MODE_WAIT_FOR_REQ;
    uint8 mitm = TRUE;
    uint8 ioCap = GAPBOND_IO_CAP_DISPLAY_ONLY;
    uint8 bonding = TRUE;
    GAPBondMgr_SetParameter( GAPBOND_DEFAULT_PASSCODE, sizeof ( uint32 ), &passkey );
    GAPBondMgr_SetParameter( GAPBOND_PAIRING_MODE, sizeof ( uint8 ), &pairMode );
    GAPBondMgr_SetParameter( GAPBOND_MITM_PROTECTION, sizeof ( uint8 ), &mitm );
    GAPBondMgr_SetParameter( GAPBOND_IO_CAPABILITIES, sizeof ( uint8 ), &ioCap );
    GAPBondMgr_SetParameter( GAPBOND_BONDING_ENABLED, sizeof ( uint8 ), &bonding );
  }  

  // Initialize GATT attributes
  GGS_AddService( GATT_ALL_SERVICES );         // GAP
  GATTServApp_AddService( GATT_ALL_SERVICES ); // GATT attributes
  DevInfo_AddService( ); // device information service
  
  HRM_AddService( GATT_ALL_SERVICES ); // heart rate monitor service
  HRM_RegisterAppCBs( &hrServCBs );
  
  Battery_AddService(GATT_ALL_SERVICES); // battery service
  Battery_RegisterAppCBs(&battServCBs);
  
  ECG_AddService(GATT_ALL_SERVICES); // ecg service
  ECG_RegisterAppCBs( &ecgServCBs );  
  
  // set characteristic in heart rate service
  {
    uint8 sensLoc = HRM_SENS_LOC_CHEST;
    HRM_SetParameter( HRM_SENS_LOC, sizeof ( uint8 ), &sensLoc );
  }
  
  // set characteristic in ecg service
  {
    uint16 ecg1mVCali = ECG_1MV_CALI_VALUE;
    ECG_SetParameter( ECG_1MV_CALI, sizeof ( uint16 ), &ecg1mVCali );
    ECG_SetParameter( ECG_SAMPLE_RATE, sizeof ( uint16 ), &SAMPLERATE );
    ECG_SetParameter( ECG_WORK_MODE, sizeof ( uint8 ), &mode );
  }    
  
  //在这里初始化GPIO
  //第一：所有管脚，reset后的状态都是输入加上拉
  //第二：对于不用的IO，建议不连接到外部电路，且设为输入上拉
  //第三：对于会用到的IO，就要根据具体外部电路连接情况进行有效设置，防止耗电
  initIOPin();
  
  HRFunc_Init(taskID);
  
  HCI_EXT_ClkDivOnHaltCmd( HCI_EXT_ENABLE_CLK_DIVIDE_ON_HALT );  

  // 启动设备
  osal_set_event( taskID, HRM_START_DEVICE_EVT );
}

static void setParameter(uint8 mode) 
{
    // set the connection parameter according to the ecg lock status
    uint16 desired_min_interval; // units of 1.25ms, Note: the ios device require min_interval>=20ms, max_interval>=min_interval+20
    uint16 desired_max_interval; // units of 1.25ms, Note: the ios device require max_interval*(1+latency)<=2s
    uint16 desired_slave_latency; // Note: the ios device require the slave latency <=4
    uint16 desired_conn_timeout; // units of 10ms, Note: the ios device require the timeout <= 6s
    if(mode == MODE_HR)
    {
      desired_min_interval = HR_MODE_MIN_INTERVAL;
      desired_max_interval = HR_MODE_MAX_INTERVAL;
      desired_slave_latency = HR_MODE_SLAVE_LATENCY;
      desired_conn_timeout = HR_MODE_CONNECT_TIMEOUT;
      SAMPLERATE = HR_MODE_SAMPLERATE;
    }
    else
    {
      desired_min_interval = ECG_MODE_MIN_INTERVAL;
      desired_max_interval = ECG_MODE_MAX_INTERVAL;
      desired_slave_latency = ECG_MODE_SLAVE_LATENCY;
      desired_conn_timeout = ECG_MODE_CONNECT_TIMEOUT;  
      SAMPLERATE = ECG_MODE_SAMPLERATE;
    }
    GAPRole_SetParameter( GAPROLE_MIN_CONN_INTERVAL, sizeof( uint16 ), &desired_min_interval );
    GAPRole_SetParameter( GAPROLE_MAX_CONN_INTERVAL, sizeof( uint16 ), &desired_max_interval );
    GAPRole_SetParameter( GAPROLE_SLAVE_LATENCY, sizeof( uint16 ), &desired_slave_latency );
    GAPRole_SetParameter( GAPROLE_TIMEOUT_MULTIPLIER, sizeof( uint16 ), &desired_conn_timeout );  
}

// 初始化IO管脚
static void initIOPin()
{
  // 全部设为GPIO
  P0SEL = 0; 
  P1SEL = 0; 
  P2SEL = 0; 

  // 全部设为输出低电平
  P0DIR = 0xFF; 
  P1DIR = 0xFF; 
  P2DIR = 0x1F; 

  P0 = 0; 
  P1 = 0;   
  P2 = 0; 
}

extern uint16 HRM_ProcessEvent( uint8 task_id, uint16 events )
{
  VOID task_id; // OSAL required parameter that isn't used in this function
  uint8 mode;

  if ( events & SYS_EVENT_MSG )
  {
    uint8 *pMsg;

    if ( (pMsg = osal_msg_receive( taskID )) != NULL )
    {
      processOSALMsg( (osal_event_hdr_t *)pMsg );

      // Release the OSAL message
      VOID osal_msg_deallocate( pMsg );
    }

    // return unprocessed events
    return (events ^ SYS_EVENT_MSG);
  }

  if ( events & HRM_START_DEVICE_EVT )
  {    
    // Start the Device
    VOID GAPRole_StartDevice( &gapStateCBs );

    // Start Bond Manager
    VOID GAPBondMgr_Register( &bondCBs );

    return ( events ^ HRM_START_DEVICE_EVT );
  }
  
  if ( events & HRM_HR_PERIODIC_EVT )
  {
    if(gapProfileState == GAPROLE_CONNECTED)
    {
      HRFunc_SendHRPacket(gapConnHandle);
      osal_start_timerEx( taskID, HRM_HR_PERIODIC_EVT, HR_NOTI_PERIOD );
    }      

    return (events ^ HRM_HR_PERIODIC_EVT);
  }
  
  if ( events & HRM_BATT_PERIODIC_EVT )
  {
    if (gapProfileState == GAPROLE_CONNECTED)
    {
      Battery_MeasLevel(gapConnHandle);
      osal_start_timerEx( taskID, HRM_BATT_PERIODIC_EVT, BATT_NOTI_PERIOD );
    }

    return (events ^ HRM_BATT_PERIODIC_EVT);
  }
  
  if ( events & HRM_ECG_NOTI_EVT )
  {
    if (gapProfileState == GAPROLE_CONNECTED)
    {
      HRFunc_SendEcgPacket(gapConnHandle);
    }

    return (events ^ HRM_ECG_NOTI_EVT);
  } 
  
  if ( events & HRM_MODE_CHANGED_EVT )
  {
    if (gapProfileState == GAPROLE_CONNECTED)
    {
      ECG_GetParameter(ECG_WORK_MODE, &mode);
      setParameter(mode);      
      ECG_SetParameter( ECG_SAMPLE_RATE, sizeof ( uint16 ), &SAMPLERATE );
      GAPRole_TerminateConnection();
    }

    return (events ^ HRM_MODE_CHANGED_EVT);
  }   
  
  // Discard unknown events
  return 0;
}

static void processOSALMsg( osal_event_hdr_t *pMsg )
{
  switch ( pMsg->event )
  {
    default:
      // do nothing
      break;
  }
}

static void gapStateCB( gaprole_States_t newState )
{
  // connected
  if( newState == GAPROLE_CONNECTED)
  {
    // Get connection handle
    GAPRole_GetParameter( GAPROLE_CONNHANDLE, &gapConnHandle );
    
    delayus(1000);
    ADS1x9x_PowerUp(); 
    delayus(1000);
    ADS1x9x_StandBy();  
    delayus(1000);
  }
  // disconnected
  else if(gapProfileState == GAPROLE_CONNECTED && 
            newState != GAPROLE_CONNECTED)
  {
    stopEcgSampling();
    HRFunc_SetHRCalcing(false);
    HRFunc_SetEcgSending(false);
    VOID osal_stop_timerEx( taskID, HRM_HR_PERIODIC_EVT ); 
    VOID osal_stop_timerEx( taskID, HRM_BATT_PERIODIC_EVT );
    //initIOPin();
    ADS1x9x_PowerDown();
  }
  // if started
  else if (newState == GAPROLE_STARTED)
  {
    // Set the system ID from the bd addr
    uint8 systemId[DEVINFO_SYSTEM_ID_LEN];
    GAPRole_GetParameter(GAPROLE_BD_ADDR, systemId);
    
    // shift three bytes up
    systemId[7] = systemId[5];
    systemId[6] = systemId[4];
    systemId[5] = systemId[3];
    
    // set middle bytes to zero
    systemId[4] = 0;
    systemId[3] = 0;
    
    DevInfo_SetParameter(DEVINFO_SYSTEM_ID, DEVINFO_SYSTEM_ID_LEN, systemId);
  }
  
  gapProfileState = newState;
}

static void hrServiceCB( uint8 event )
{
  switch (event)
  {
    case HRM_HR_NOTI_ENABLED:
      startEcgSampling();  
      HRFunc_SetHRCalcing(true);
      osal_start_timerEx( taskID, HRM_HR_PERIODIC_EVT, HR_NOTI_PERIOD);
      break;
        
    case HRM_HR_NOTI_DISABLED:
      stopEcgSampling();
      HRFunc_SetHRCalcing(false);
      osal_stop_timerEx( taskID, HRM_HR_PERIODIC_EVT ); 
      break;

    case HRM_CTRL_PT_SET:
      
      break;
      
    default:
      // Should not get here
      break;
  }
}

// start ecg Sampling
static void startEcgSampling( void )
{  
  if(status == STATUS_ECG_STOP) 
  {
    status = STATUS_ECG_START;
    HRFunc_SetEcgSampling(true);
  }
}

// stop ecg Sampling
static void stopEcgSampling( void )
{  
  if(status == STATUS_ECG_START)
  {
    status = STATUS_ECG_STOP;
    HRFunc_SetEcgSampling(false);
  }
}

static void battServiceCB( uint8 event )
{
  if (event == BATTERY_LEVEL_NOTI_ENABLED)
  {
    // if connected start periodic measurement
    if (gapProfileState == GAPROLE_CONNECTED)
    {
      osal_start_timerEx( taskID, HRM_BATT_PERIODIC_EVT, BATT_NOTI_PERIOD );
    } 
  }
  else if (event == BATTERY_LEVEL_NOTI_DISABLED)
  {
    // stop periodic measurement
    osal_stop_timerEx( taskID, HRM_BATT_PERIODIC_EVT );
  }
}

static void ecgServiceCB( uint8 event )
{
  uint8 mode;
  switch (event)
  {
    case ECG_PACK_NOTI_ENABLED:
      HRFunc_SetEcgSending(true);
      break;
        
    case ECG_PACK_NOTI_DISABLED:
      HRFunc_SetEcgSending(false);
      break;
      
    case ECG_WORK_MODE_CHANGED:
      ECG_GetParameter( ECG_WORK_MODE, &mode );
      if(osal_snv_write(NVID_WORK_MODE, sizeof(uint8), (uint8*)&mode) == SUCCESS)
      {
        osal_set_event(taskID, HRM_MODE_CHANGED_EVT);
      }
      break;
      
    default:
      // Should not get here
      break;
  }
}