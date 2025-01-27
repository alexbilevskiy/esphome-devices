// <<<<<<<<<<<<<<<<<<<< EHU32.ino >>>>>>>>>>>>>>>>>>>>

// internal CAN queue constants
#define QUEUE_LENGTH 100
#define MSG_SIZE sizeof(twai_message_t)

TaskHandle_t canReceiveTaskHandle, canDisplayTaskHandle, canProcessTaskHandle, canTransmitTaskHandle, canWatchdogTaskHandle, canAirConMacroTaskHandle, canMessageDecoderTaskHandle, eventHandlerTaskHandle;
QueueHandle_t canRxQueue, canTxQueue, canDispQueue;
SemaphoreHandle_t CAN_MsgSemaphore=NULL, BufferSemaphore=NULL;
// TWAI driver stuff
uint32_t alerts_triggered;
// CAN related flags
volatile bool DIS_forceUpdate=0, CAN_MessageReady=0, CAN_prevTxFail=0, CAN_flowCtlFail=0, CAN_speed_recvd=0, CAN_coolant_recvd=0, CAN_voltage_recvd=0, CAN_new_dataSet_recvd=0, CAN_measurements_requested=0, disp_mode_changed=0, CAN_allowDisplay=0;
// measurement related flags
volatile bool ECC_present=0;
// global bluetooth flags
volatile bool ehu_started=0, a2dp_started=0, bt_connected=0, bt_state_changed=0, bt_audio_playing=0, audio_state_changed=0, OTA_begin=0;
// body data
int CAN_data_speed=0, CAN_data_rpm=0;
float CAN_data_coolant=0, CAN_data_voltage=0;
// data buffers
char DisplayMsg[512], CAN_MsgArray[64][8], title_buffer[64], artist_buffer[64], album_buffer[64];
char coolant_buffer[32], speed_buffer[32], voltage_buffer[32];
// display mode 0 -> song metadata and general status messages, 1 -> body data, 2 -> single-line body data, -1 -> prevent screen updates
int disp_mode=-1;
// time to compare against
unsigned long last_millis=0, last_millis_req=0, last_millis_disp=0;

//void canReceiveTask(void* pvParameters);
void canTransmitTask(void* pvParameters);
//void canProcessTask(void* pvParameters);
void canDisplayTask(void* pvParameters);
//void canWatchdogTask(void* pvParameters);
//void canAirConMacroTask(void* pvParameters);
//void OTAhandleTask(void* pvParameters);
void prepareMultiPacket(int bytes_processed, char* buffer_to_read);
int processDisplayMessage(char* upper_line_buffer, char* middle_line_buffer, char* lower_line_buffer);
void sendMultiPacket();
void sendMultiPacketData();
void requestMeasurementBlocks();

// processes data based on the current value of disp_mode or prints one-off messages by specifying the data in arguments; message is then transmitted right away
// it acts as a bridge between UTF-8 text data and the resulting CAN messages meant to be transmitted to the display
void writeTextToDisplay(bool disp_mode_override=0, char* up_line_text=nullptr, char* mid_line_text=nullptr, char* low_line_text=nullptr){             // disp_mode_override exists as a simple way to print one-off messages (like board status, errors and such)
  xSemaphoreTake(CAN_MsgSemaphore, portMAX_DELAY);      // take the semaphore as a way to prevent any transmission when the message structure is being written
  xSemaphoreTake(BufferSemaphore, portMAX_DELAY);       // we take both semaphores, since this task specifically interacts with both the internal data buffers and the CAN message buffer
  if(!disp_mode_override){
    if(disp_mode==0 && (album_buffer[0]!='\0' || title_buffer[0]!='\0' || artist_buffer[0]!='\0')){         // audio metadata mode
      prepareMultiPacket(processDisplayMessage(album_buffer, title_buffer, artist_buffer), DisplayMsg);               // prepare a 3-line message (audio Title, Album and Artist)
    } else {
      if(disp_mode==1){                                     // vehicle data mode (3-line)
        prepareMultiPacket(processDisplayMessage(coolant_buffer, speed_buffer, voltage_buffer), DisplayMsg);               // vehicle data buffer
      }
      if(disp_mode==2){                                     // coolant mode (1-line)
        prepareMultiPacket(processDisplayMessage(nullptr, coolant_buffer, nullptr), DisplayMsg);               // vehicle data buffer (single line)
      }
    }
  } else {                                   // overriding buffers
    prepareMultiPacket(processDisplayMessage(up_line_text, mid_line_text, low_line_text), DisplayMsg);
  }
  xSemaphoreGive(CAN_MsgSemaphore);
  xSemaphoreGive(BufferSemaphore);        // releasing semaphores
  DIS_forceUpdate=0;
  vTaskResume(canDisplayTaskHandle);        // resume display task, it will suspend itself after its job is done
}

// this task handles events and output to display in context of events, such as new data in buffers or A2DP events
void eventHandlerTask(void *pvParameters){
  while(1){
    if(disp_mode==1 && ehu_started){                           // if running in measurement block mode, check time and if enough time has elapsed ask for new data
      if(disp_mode_changed){
        disp_mode_changed=0;
        writeTextToDisplay(1, nullptr, "No data yet...", nullptr);      // print a status message that will stay if display/ecc are not responding
      }
      if((last_millis_req+250)<millis()){
        requestMeasurementBlocks();
        last_millis_req=millis();
      }
      if(((last_millis_disp+250)<millis()) && CAN_new_dataSet_recvd){               // print new data if it has arrived
        CAN_new_dataSet_recvd=0;
        writeTextToDisplay();
        last_millis_disp=millis();
      }
    }

    if(disp_mode==2 && ehu_started){                  // single line measurement mode only provides coolant temp.
      if(disp_mode_changed){
        disp_mode_changed=0;
        writeTextToDisplay(1, nullptr, "No data yet...", nullptr);      // print a status message that will stay if display/ecc are not responding
      }
      if((last_millis_req+3000)<millis()){        // since we're only updating coolant data, I'd say that 3 secs is a fair update rate
        requestMeasurementBlocks();
        last_millis_req=millis();
      }
      if(((last_millis_disp+3000)<millis()) && CAN_coolant_recvd){
        CAN_coolant_recvd=0;
        writeTextToDisplay();
        last_millis_disp=millis();
      }
    }

    if(DIS_forceUpdate && disp_mode==0 && CAN_allowDisplay){                       // handles data processing for A2DP AVRC data events
      writeTextToDisplay();
    }

//    A2DP_EventHandler();          // process bluetooth and audio flags set by interrupt callbacks
    vTaskDelay(10);
  }
}
