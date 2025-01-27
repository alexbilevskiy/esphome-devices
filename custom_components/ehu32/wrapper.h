// <<<<<<<<<<<<<<<<<<<< init for esphome, mostly copied from EHU32::setup >>>>>>>>>>>>>>>>>>>>

void run() {
    a2dp_started=1;
    disp_mode=0;
    ehu_started=1;
    bt_connected=1;

    twai_init();
    CAN_MsgSemaphore=xSemaphoreCreateMutex();  // as stuff is done asynchronously, we need to make sure that the message will not be transmitted when its being written to
    BufferSemaphore=xSemaphoreCreateMutex();    // CAN_MsgSemaphore is used when encoding the message and transmitting it, while BufferSemaphore is used when acquiring new data or encoding the message

    canRxQueue=xQueueCreate(QUEUE_LENGTH, MSG_SIZE);        // internal EHU32 queue for messages to be handled by the canProcessTask
    canTxQueue=xQueueCreate(QUEUE_LENGTH, MSG_SIZE);        // internal EHU32 queue for messages to be transmitted
    canDispQueue=xQueueCreate(255, sizeof(uint8_t));        // queue used for handling of raw ISO 15765-2 data that's meant for the display (Aux string detection)

    xTaskCreate(canReceiveTask, "CANbusReceiveTask", 4096, NULL, 1, &canReceiveTaskHandle);
    xTaskCreate(canTransmitTask, "CANbusTransmitTask", 4096, NULL, 2, &canTransmitTaskHandle);
    xTaskCreate(canProcessTask, "CANbusMessageProcessor", 8192, NULL, 3, &canProcessTaskHandle);
    xTaskCreate(canDisplayTask, "DisplayUpdateTask", 8192, NULL, 3, &canDisplayTaskHandle);
//    vTaskSuspend(canDisplayTaskHandle);
//    xTaskCreatePinnedToCore(canWatchdogTask, "WatchdogTask", 2048, NULL, 20, &canWatchdogTaskHandle, 0);
    xTaskCreatePinnedToCore(canMessageDecoder, "MessageDecoder", 2048, NULL, 5, &canMessageDecoderTaskHandle, 0);
//    vTaskSuspend(canMessageDecoderTaskHandle);
    xTaskCreatePinnedToCore(canAirConMacroTask, "AirConMacroTask", 2048, NULL, 10, &canAirConMacroTaskHandle, 0);
    vTaskSuspend(canAirConMacroTaskHandle);       // Aircon macro task exists solely to execute simulated button presses asynchronously, as such it is only started when needed

    xTaskCreatePinnedToCore(eventHandlerTask, "eventHandler", 8192, NULL, 4, &eventHandlerTaskHandle, 0);
}

