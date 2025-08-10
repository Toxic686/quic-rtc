#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <chrono>

#include "rtc/rtc.hpp"

using namespace std::chrono_literals;

int main() {
    try {
        // Initialize the library
        rtc::Init();
        
        // Create peer connections
        rtc::Configuration config1, config2;
        
        // Enable QUIC transport (this would be a new feature)
        config1.enableQuicTransport = true;
        config2.enableQuicTransport = true;
        
        auto pc1 = std::make_shared<rtc::PeerConnection>(config1);
        auto pc2 = std::make_shared<rtc::PeerConnection>(config2);
        
        // Set up data channel callbacks for pc1
        pc1->onDataChannel([](std::shared_ptr<rtc::DataChannel> dc) {
            std::cout << "pc1: Received data channel: " << dc->label() << std::endl;
            
            dc->onOpen([]() {
                std::cout << "pc1: Data channel opened" << std::endl;
            });
            
            dc->onMessage([](rtc::message_variant data) {
                if (std::holds_alternative<std::string>(data)) {
                    std::cout << "pc1: Received message: " << std::get<std::string>(data) << std::endl;
                } else {
                    std::cout << "pc1: Received binary message of size: " 
                              << std::get<rtc::binary>(data).size() << std::endl;
                }
            });
            
            dc->onClosed([]() {
                std::cout << "pc1: Data channel closed" << std::endl;
            });
        });
        
        // Set up data channel callbacks for pc2
        pc2->onDataChannel([](std::shared_ptr<rtc::DataChannel> dc) {
            std::cout << "pc2: Received data channel: " << dc->label() << std::endl;
            
            dc->onOpen([]() {
                std::cout << "pc2: Data channel opened" << std::endl;
            });
            
            dc->onMessage([](rtc::message_variant data) {
                if (std::holds_alternative<std::string>(data)) {
                    std::cout << "pc2: Received message: " << std::get<std::string>(data) << std::endl;
                } else {
                    std::cout << "pc2: Received binary message of size: " 
                              << std::get<rtc::binary>(data).size() << std::endl;
                }
            });
            
            dc->onClosed([]() {
                std::cout << "pc2: Data channel closed" << std::endl;
            });
        });
        
        // Create data channel on pc1
        auto dc1 = pc1->createDataChannel("test-quic-channel");
        
        dc1->onOpen([]() {
            std::cout << "pc1: Data channel created and opened" << std::endl;
        });
        
        dc1->onMessage([](rtc::message_variant data) {
            if (std::holds_alternative<std::string>(data)) {
                std::cout << "pc1: Received message: " << std::get<std::string>(data) << std::endl;
            } else {
                std::cout << "pc1: Received binary message of size: " 
                          << std::get<rtc::binary>(data).size() << std::endl;
            }
        });
        
        // Set up connection state callbacks
        pc1->onLocalDescription([](rtc::Description description) {
            std::cout << "pc1: Local description: " << description << std::endl;
        });
        
        pc1->onLocalCandidate([](rtc::Candidate candidate) {
            std::cout << "pc1: Local candidate: " << candidate << std::endl;
        });
        
        pc2->onLocalDescription([](rtc::Description description) {
            std::cout << "pc2: Local description: " << description << std::endl;
        });
        
        pc2->onLocalCandidate([](rtc::Candidate candidate) {
            std::cout << "pc2: Local candidate: " << candidate << std::endl;
        });
        
        // Exchange descriptions
        pc1->setLocalDescription();
        pc2->setLocalDescription();
        
        // Wait for descriptions to be generated
        std::this_thread::sleep_for(100ms);
        
        // Exchange the descriptions
        pc2->setRemoteDescription(pc1->localDescription());
        pc1->setRemoteDescription(pc2->localDescription());
        
        // Wait for connection to be established
        std::this_thread::sleep_for(1s);
        
        // Send a test message
        if (dc1->isOpen()) {
            std::cout << "Sending test message..." << std::endl;
            dc1->send("Hello QUIC DataChannel!");
            
            // Send a binary message
            rtc::binary binaryData = {0x01, 0x02, 0x03, 0x04, 0x05};
            dc1->send(binaryData);
        } else {
            std::cout << "Data channel not open yet" << std::endl;
        }
        
        // Wait for messages to be processed
        std::this_thread::sleep_for(2s);
        
        // Close the connection
        dc1->close();
        pc1->close();
        pc2->close();
        
        std::cout << "Test completed successfully!" << std::endl;
        
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return -1;
    }
    
    return 0;
} 