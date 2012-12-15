//
//  LeapMIDIX
//
//  Created by Mischa Spiegelmock on 12/1/12.
//  Copyright (c) 2012 DBA int80. All rights reserved.
//

#include "Device.h"
#include <CoreMIDI/CoreMIDI.h>
#include <CoreMIDI/MIDIServices.h>

static void fatal(const char *msg);

namespace LeapMIDIX {
    Device::Device() {
        pthread_mutex_init(&messageQueueMutex, NULL);
        pthread_cond_init(&messageQueueCond, NULL);

        packetListSize = 512; // buffer size for midi packet messages
        deviceClient = NULL;
        deviceEndpoint = NULL;
        midiPacketList = NULL;
    }

    void Device::init() {
        initPacketList();
        createDevice();
        
        // start message sending queue
        int res = pthread_create(&messageQueueThread, NULL, _messageSendingThreadEntry, this);
        if (res) {
            std::cerr << "pthread_create failed " << res << std::endl;
            exit(1);
        }
    }
    
    void Device::initPacketList() {
        if (midiPacketList) {
            free(midiPacketList);
            midiPacketList = NULL;
        }
        
        midiPacketList = (MIDIPacketList *)malloc(packetListSize * sizeof(char));
        curPacket = MIDIPacketListInit(midiPacketList);
    }

    Device::~Device() {
        if (deviceEndpoint)
            MIDIEndpointDispose(deviceEndpoint);
        if (deviceClient)
            MIDIDeviceDispose(deviceClient);
        if (midiPacketList)
            free(midiPacketList);
        
        if (messageQueueThread)
            pthread_cancel(messageQueueThread);
        
        pthread_mutex_destroy(&messageQueueMutex);
        pthread_cond_destroy(&messageQueueCond);
        
        std::cout << "closed down device\n";
    }

    void Device::createDevice() {
        OSStatus result;
        
        result = MIDIClientCreate(CFSTR("LeapMIDIX"), NULL, NULL, &deviceClient);
        if (result)
            fatal("Failed to create MIDI client");
        
        result = MIDISourceCreate(deviceClient, CFSTR("LeapMIDIX Control"), &deviceEndpoint);
        if (result)
            fatal("Failed to create MIDI source");
        
    }

    // "send" a packet, really pretends that our virtual device source received a packet
    OSStatus Device::send(const MIDIPacketList *pktlist) {
        // send current packet list
        OSStatus res = MIDIReceived(deviceEndpoint, pktlist);
        
        // reinitialize packet list, MIDIReceived does not appear to flush the list
        // none of this is really documented but this seems to work ok
        // FIXME: probably not thread-safe, if a frame callback is called while this is happening
        initPacketList();

        return res;
    }
    
    // control = MIDI control #, 0-119
    // value = MIDI control message value, 0-127
    void Device::writeControl(LeapMIDI::midi_control_index control, LeapMIDI::midi_control_value value) {
        assert(control < 120);
        assert(value <= 127);
        
        // assign channel value
        // (hardcoded to channel 0 - Registered Parameter LSB)
        unsigned char channelBase = 0xB0;
        unsigned char channel = 0;
        unsigned char midiChannel = channelBase + channel;
        
        // assign control selector
        unsigned char controlBase = 0x00;
        unsigned char midiControl = controlBase + control;
        
        // build midi packet
        Byte packetOut[3];
        packetOut[0] = midiChannel;
        packetOut[1] = midiControl;
        packetOut[2] = value;
        
        // add packet to packet list
        curPacket = MIDIPacketListAdd(midiPacketList, packetListSize, curPacket, 0, 3, packetOut);
        if (! curPacket) {
            std::cerr << "Buffer overrun on midi packet list\n";
            exit(1);
        }
        
//        std::cout << "Emitted channel " << (int)channel << ", control: " <<
//            (int)midiControl << ", value: " << (int)value << std::endl;
        
        // "send" packet
        this->send(midiPacketList);
    }
    
    void *Device::messageSendingThreadEntry() {
        std::cout << "messageSendingThreadEntry\n";
        struct timeval tv;
        struct timespec ts;

        while (1) {
            pthread_testcancel();
            
            // CHECK IF QUEUE IS EMPTY, IF NOT DON'T DO TIMEWAIT
            // (condvar will not be broadcasted if message is added right here)
            
            // acquire mutex once there is a message waiting for us
            gettimeofday(&tv, NULL);
            ts.tv_sec = tv.tv_sec + 2; // timeout 2s
            ts.tv_nsec = 0;
            int res = pthread_cond_timedwait(&messageQueueCond, &messageQueueMutex, &ts);
            if (res == ETIMEDOUT) {
                // no message was waiting
                std::cout << "ETIMEDOUT\n";
                continue;
            }
            if (res != 0) {
                std::cerr << "unexpected pthread_cond_timedwait retval=" << res << std::endl;
                exit(1);
                continue;
            }
            
            if (midiMessageQueue.empty()) {
                pthread_mutex_unlock(&messageQueueMutex);
                continue;
            }
            
            // copy messages from shared queue into thread-local copy
            // (is there a cleaner way to do this?)
            std::queue<midi_message> queueCopy;
            while (! midiMessageQueue.empty()) {
                midi_message msg = midiMessageQueue.front();
                queueCopy.push(msg);
                midiMessageQueue.pop();
            }
            
            // unlock
            if (pthread_mutex_unlock(&messageQueueMutex) != 0) {
                std::cerr << "message queue mutex unlock failure\n";
                exit(1);
            }
            
            writeControlMessages(queueCopy);
        }
        
        return NULL;
    }
    
    void Device::writeControlMessages(std::queue<midi_message> &messages) {
        struct timeval tv;

        while (! messages.empty()) {
            midi_message msg = messages.front();
            messages.pop();
            
            // figure out when this packet was added
            gettimeofday(&tv, NULL);
            double elapsedTime = (tv.tv_sec - msg.timestamp.tv_sec) * 1000.0;      // sec to ms
            elapsedTime += (tv.tv_usec - msg.timestamp.tv_usec) / 1000.0;   // us to ms
            if (elapsedTime > 2) {
                std::cerr << "Warning, MIDI control message latency of " << elapsedTime << "ms detected.\n";
                continue;
            }
            
            // we have data to send. i think this blocks
            writeControl(msg.control_index, msg.control_value);
        }
    }
    
    void Device::addControlMessage(LeapMIDI::midi_control_index controlIndex, LeapMIDI::midi_control_value controlValue) {
        pthread_mutex_lock(&messageQueueMutex);
        midi_message msg;
        msg.control_index = controlIndex;
        msg.control_value = controlValue;
        gettimeofday(&msg.timestamp, NULL);
        midiMessageQueue.push(msg);
        pthread_mutex_unlock(&messageQueueMutex);
        pthread_cond_signal(&messageQueueCond);
    }
}

///

static void fatal(const char *msg) {
    fprintf(stderr, "Fatal error: %s\n", msg);
    exit(1);
}

