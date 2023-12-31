#include <BlueteethInternalNetworkStack.h>

extern BlueteethBaseStack * internalNetworkStackPtr;

#ifdef TIME_STREAMING
uint32_t streamTime; //DEBUG (REMOVE LATER)
#endif

void packDataStream(uint8_t * framedData, int dataLength, deque<uint8_t> & payloadBuffer){

    uint8_t select_lower;
    //Converting dataLength to an integer multiple of the frame size
  
    size_t finalFrameEnd = ceil( (double) dataLength / PAYLOAD_SIZE ) * FRAME_SIZE;
    size_t finalPayloadBytePosition = dataLength/PAYLOAD_SIZE * FRAME_SIZE + 1 + (dataLength % PAYLOAD_SIZE)/BYTES_PER_ROTATION + dataLength % PAYLOAD_SIZE; //Size of frames prior to the final frame + Frame start sentinel + Bytes from complete rotations of final frame + Bytes from incomplete rotation of the final frame.

    for(volatile int frame = 0; frame < finalFrameEnd; frame += FRAME_SIZE){
        framedData[frame] = FRAME_START_SENTINEL;
        for (volatile int rotation = 0; rotation < (ROTATIONS_PER_FRAME * BYTES_PER_ROTATION) ; rotation += BYTES_PER_ROTATION){
          select_lower = 0b00000001; //used to select the lower portion of the unpacked byte;
          framedData[frame + rotation + 1] = 0; //Necessary as the first byte is some random number prior to starting algorithm 
          for(volatile int byte = 1; byte < BYTES_PER_ROTATION; ++byte){ //pre-increment is technically faster as there isn't a copy of the var made (so doing ++byte rather than byte++)

              if ((frame + rotation + byte) < finalPayloadBytePosition) {
                framedData[frame + rotation + byte] += payloadBuffer.front() >> byte;
                framedData[frame + rotation + byte + 1] = (select_lower & payloadBuffer.front()) << (7 - byte);           
                payloadBuffer.pop_front();
                select_lower = (select_lower << 1) + 1;
              }
              else {         
                framedData[frame + rotation + byte + 1] = FRAME_PADDING_SENTINEL;
                // cout << "Placing pading...." << endl;
                if (frame + rotation + byte + 1 >= finalFrameEnd){
                    Serial.println("Something went wrong trying to package a frame...");
                    return;
                }
              }
          }
        }
    }
}

#include <stdexcept>

void unpackDataStream(const uint8_t * dataStream, const int dataStreamLength, deque<uint8_t> & dataBuffer, HardwareSerial * dataPlane){
    
    uint8_t select_lower;
    uint8_t select_upper;
    vector<uint8_t> framePayload;  

    const auto outOfBoundsCheck = [dataStreamLength] (int inc1, int inc2) -> bool {
        if ((inc1 + inc2) >= dataStreamLength){ 
            return true;
        }
        else {
            return false;
        } 
    };

    auto before = dataBuffer.size();

    volatile int cnt = 0;

loop_start:

    while (cnt < dataStreamLength ){

        framePayload.clear();

        if (dataStream[cnt++] == FRAME_START_SENTINEL){ //Don't begin unpacking until the sentinal character is found 

            if (outOfBoundsCheck(cnt, 0)) {
                //Will occur if the last byte received was a FRAME_START_SENTINEL
                return;
            }

            for (volatile int rotation = 0; rotation < ROTATIONS_PER_FRAME; ++rotation){ 
                
                select_upper = 0b01111111; //Used to select the upper portion of the unpacked byte 
                select_lower = 0b01000000; //Used to select the lower portion of the unpacked byte
                
                for(volatile int byte = 0; byte < (BYTES_PER_ROTATION - 1); ++byte){
                    
                    if (outOfBoundsCheck(cnt, byte)){
                        // Serial.printf("%d -> byte = %d, rotation = %d (dataBufferSize = %d, bytes available = %d)\n\r", cnt, byte, rotation, dataBuffer.size(), dataPlane -> available());
                        // flushSerialBuffer(dataPlane);
                        return; //Corruption occurred
                    }

                    switch(dataStream[cnt + byte + 1]){
                        case FRAME_START_SENTINEL:
                          //CORRUPTION OCCURRED
                          goto loop_start;
                      
                        case FRAME_PADDING_SENTINEL: //If you see a start sentinel before the end of a frame, the frame was corrupted.    
                          goto loop_start; //Don't bother correcting until the problem is solved

                        default:
                            // if (packedData[cnt + byte] > 0b10000000){
                            //     Serial.print("A bad byte was detected in a frame...\n\r");
                            // }

                            framePayload.push_back(
                                ((dataStream[cnt + byte] & select_upper) << (byte + 1)) + 
                                ((dataStream[cnt + byte + 1] & select_lower) >> (6 - byte))
                            ); 
                            select_upper = select_upper >> 1;
                            select_lower += 1 << (5 - byte);
                    }
                }
                cnt += BYTES_PER_ROTATION;
            }

        }

loop_end:

    if ((framePayload.size() % 4) != 0){
        Serial.println("Corruption detected");

        while (((framePayload.size() % 4)) != 0){
            framePayload.pop_back();
        }
    }
    if (framePayload.size() > 0)
    try{
        dataBuffer.insert(dataBuffer.end(), framePayload.begin(), framePayload.end());
    }
    catch (const std::bad_alloc &e){
        Serial.printf("Bad Allocation occurred (size was %d): %s", e.what(), dataBuffer.size());
    }
    catch (const std::out_of_range &e) {
        Serial.printf("Out of Range occurred: %s", e.what());
    } 
    catch (const std::exception &e) {
        Serial.printf("Exception occurred: %s", e.what());
    }
    catch(...){
        Serial.printf("This was the offending line. The sizes were as follows: dataBuffer = %d, framePayload = %d", dataBuffer.size(), framePayload.size());
        throw;
    }
  }

  return;
}


void inline flushSerialBuffer(HardwareSerial * serial){
    while (serial -> available() > 0){
        serial -> read();
    }
}


/* Grab contents from serial buffer and package them into a blueteeth packet
*
*  @serial - The serial port being read from
*  @return - The blueteeth packet received from the buffer
*/
BlueteethPacket retrievePacketFromBuffer(HardwareSerial * serial){

    //Have to temporarily grab the data form the UART buffer into a byte buffer (so it can be packaged in the packet struct)
    // uint8_t buffer [sizeof(blueTeethPacket)];
    // serial -> readBytes(buffer, sizeof(buffer));
    
    //Package the packet struct with data from buffer
    BlueteethPacket retrievedPacket;
    retrievedPacket.tokenFlag = serial -> read();
    retrievedPacket.srcAddr = serial -> read();
    retrievedPacket.dstAddr = serial -> read();
    retrievedPacket.type = serial -> read();
    for (int i = 0; i < MAX_PAYLOAD_SIZE; i++){
        retrievedPacket.payload[i] = serial -> read();
    }
    return retrievedPacket;

}

void uartFrameReceived(){
    
    int bytesInBuffer = internalNetworkStackPtr -> controlPlane -> available();

    //Don't try to package packet unless there's enough data on the buffer to do so
    if(bytesInBuffer < sizeof(BlueteethPacket)){
        // Serial.print("Not enough bits for a packet\n\r"); //DEBUG STATEMENT 
        return;
    }  
    //If there are extra bytes in the buffer, something is wrong, and the buffer has been corrupted.
    /*  
        This seems to prevent corruption as the serial callback doesn't get called EVERY time 
        a new frame is received, but after a burst of frames ends. Maybe move into the prior conditional. 
    */
    else if ((bytesInBuffer % sizeof(BlueteethPacket)) != 0 ) 
    {
        // Serial.print("Buffer corrupted\n\r");
        flushSerialBuffer(internalNetworkStackPtr -> controlPlane);
        return;
    }      
    
    BlueteethPacket receivedPacket = retrievePacketFromBuffer(internalNetworkStackPtr -> controlPlane);

    // Serial.printf("[%lu - Rx Packet] t = %u, src = %u, dst = %u, type = %u, payload = %s\n\r", millis(), receivedPacket.tokenFlag, receivedPacket.srcAddr, receivedPacket.dstAddr, receivedPacket.type, (char*) receivedPacket.payload); //DEBUG STATEMENT

    //Assess the packet
    //If the token was received, transmit items in queue.
    if (receivedPacket.tokenFlag == 1){
        // Serial.print("Token packet received\n\r"); //DEBUG STATEMENT 
        internalNetworkStackPtr -> tokenReceived();
    }
    else if (receivedPacket.dstAddr == 255){ //Packets with destination address 255 are intended to intialize node addresses
        Serial.print("Received initializaiton packet\n\r"); //DEBUG STATEMENT
        internalNetworkStackPtr -> initializationReceived(receivedPacket);
    }
    //If the packet was sent from this device, throw it away.
    else if (receivedPacket.srcAddr == internalNetworkStackPtr -> address){
        // Serial.print("Received my own packet\n\r"); //DEBUG STATEMENT 
        //Do nothing
    }
    else if (receivedPacket.dstAddr == 254){ //Packets with destination address 254 are intended for broadcasts
        // Serial.print("Received broadcast packet\n\r"); //DEBUG STATEMENT 
        internalNetworkStackPtr -> queuePacket(false, receivedPacket);
        xQueueSend(internalNetworkStackPtr -> receivedPacketBuffer, &receivedPacket, 0);
        vTaskResume(*(internalNetworkStackPtr -> receiveTaskCallback));
    }
    //If instead the packet was meant for this device, add it to the buffer
    else if (receivedPacket.dstAddr == internalNetworkStackPtr -> address){
        // Serial.print("Received packet for myself\n\r"); //DEBUG STATEMENT 
        xQueueSend(internalNetworkStackPtr -> receivedPacketBuffer, &receivedPacket, 0);
        vTaskResume(*(internalNetworkStackPtr -> receiveTaskCallback));
    }
    //If the packet was not meant for this device at all, send it on to the next device in the ring
    else {
        // Serial.print("Received packet for someone else\n\r"); //DEBUG STATEMENT
        internalNetworkStackPtr -> transmitPacket(receivedPacket);
    }
}


void dataStreamReceived(){
    //Making variables static to try and save time on allocating memory on each function call
    static int newBytes;
    static int currentSize;
    static bool flushToken;
    static uint8_t tmp [DATA_PLANE_SERIAL_RX_BUFFER_SIZE]; 

    // if (internalNetworkStackPtr -> getDataPlaneBytesAvailable() < 512){
    //     return;
    // }
    
    while (xSemaphoreTake(internalNetworkStackPtr -> dataBufferMutex, 0) == pdFALSE){
        //Do nothing
        // Serial.print("DATA PLANE Yielding...\n\r");
        vTaskPrioritySet(NULL, 19); //reduce to A2DP priority in order to yield to it
        vPortYield();
    }
    vTaskPrioritySet(NULL, 24); //go back to highest priority after yielding.


    if ( (internalNetworkStackPtr -> dataBuffer.size() % 4 ) != 0 ){
        Serial.print("Something went wrong...\n\r");
        internalNetworkStackPtr -> dataBuffer.clear();
    }

    newBytes = internalNetworkStackPtr -> dataPlane -> available(); //Amount available
    currentSize = internalNetworkStackPtr -> dataBuffer.size();
    
    #ifdef TIME_STREAMING
    if (internalNetworkStackPtr -> dataBuffer.size() == 0) { 
        streamTime = millis();
    }
    #endif
    
    flushToken = false;
    if ((currentSize + newBytes * PAYLOAD_SIZE / FRAME_SIZE) > MAX_DATA_BUFFER_SIZE){
        newBytes = MAX_DATA_BUFFER_SIZE - currentSize;
        flushToken = true;
        // Serial.printf("Buffer Full (%d bytes in buffer and adding %d bytes)\n\r", internalNetworkStackPtr->dataBuffer.size(), newBytes);
    }
    
    static int bytesReady;
    static int bytesProcessed; //DEBUG VARIABLE
    bytesReady = newBytes - (newBytes % FRAME_SIZE);

    internalNetworkStackPtr -> dataPlane -> readBytes(tmp, bytesReady);
    // for (int pos = 0; pos < bytesReady; pos += FRAME_SIZE){
    //     if (tmp[pos] != FRAME_START_SENTINEL){
    //         // Serial.printf("One byte dropped between %d bytes received and %d bytes received\n\r", bytesProcessed + pos - FRAME_SIZE, bytesProcessed + pos);
    //     }
    //     // Serial.printf("%u ", tmp[pos]);
    // }
    // bytesProcessed += bytesReady;


    unpackDataStream(tmp, bytesReady, internalNetworkStackPtr -> dataBuffer, internalNetworkStackPtr -> dataPlane);

    bytesProcessed = (internalNetworkStackPtr -> dataBuffer.size() - currentSize) / PAYLOAD_SIZE * FRAME_SIZE;
    if (bytesProcessed > bytesReady){
        Serial.printf("There was %d amount of bytes lost (expected = %d, actual = %d))\n\r", bytesProcessed - bytesReady, bytesProcessed, bytesReady);
    }

    if(flushToken){
        Serial.printf("Flushing the serial buffer...\n\r");
        flushSerialBuffer(internalNetworkStackPtr -> dataPlane);
    }

    #ifdef TIME_STREAMING
    if (internalNetworkStackPtr -> dataBuffer.size() >= DATA_STREAM_TEST_SIZE){ //DEBUG STATEMENT
        streamTime = millis() - streamTime;
    } 
    #endif

    internalNetworkStackPtr -> recordDataReceptionTime();

    xSemaphoreGive(internalNetworkStackPtr -> dataBufferMutex);

    // flushSerialBuffer(internalNetworkStackPtr -> dataPlane);
    // Serial.printf("Received %d bytes\n\r", newBytes);
    // Serial.printf("Received %d bytes (attempted to read %d vs. expectation of %d). There were %d bytes and now there's %d bytes in the queue (delta = %d). There are %d bytes left in the buffer.\n\r", newBytes, bytesReady,(internalNetworkStackPtr -> dataBuffer.size() - currentSize)/7*9, currentSize, internalNetworkStackPtr -> dataBuffer.size(), internalNetworkStackPtr -> dataBuffer.size() - currentSize, internalNetworkStackPtr -> dataPlane -> available()); //DEBUG STATEMENT
}