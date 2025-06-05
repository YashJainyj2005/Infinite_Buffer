#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <fstream>
#include <vector>
#include <chrono>
#include <iomanip>
#include <cmath>
#include <sstream>
#include <SFML/Graphics.hpp>
#include <SFML/System.hpp>
#include <atomic>
#include <bits/stdc++.h>
using namespace std;

// Each node contains the data to be stored in it, a flag indicating whehter full or empty and a pointer to the next node.
struct Node {
    int data;
    bool filled;
    Node* next;
    Node() : data(0), filled(false), next(nullptr) {}
};

// Custom made ticket lock

// This lock helps in introducing fairness in the synchronization process as 
// each producer/consumer is given a ticket value and is served in FIFO order
class TicketLock {
    private:
        atomic<int> next_ticket{0};
        atomic<int> now_serving{0};
    
    public:
        void lock() {
            int my_ticket = next_ticket.fetch_add(1);
            while (now_serving.load() != my_ticket) {
                this_thread::yield();  // Yield CPU to reduce contention
            }
        }
    
        void unlock() {
            now_serving.fetch_add(1);
        }

    };


// -------------------- Linked List Buffer --------------------
class LinkedListBuffer {
private:
    Node* head; // Producer writes at the head end
    Node* tail; // Consumer reads at the tail end
    const int BUFFER_SIZE = 10; // Fixed buffer size

    TicketLock ticket_lock_producer;
    mutex mutex_producer;       
    mutex mutex_consumer;      
    condition_variable cv_not_empty;       
    condition_variable cv_not_full;      

    mutex prod_stat_mutex;   
    mutex cons_stat_mutex;

    chrono::duration<double> total_produce_time{};     
    chrono::duration<double> total_consume_time{};   

    chrono::steady_clock::time_point start_time;     

public:
// Implementing circular linked list to implement finite fixed buffer
    LinkedListBuffer() {
        Node* first = new Node();   
        Node* current = first;
        
        for(int i = 1; i < BUFFER_SIZE; ++i) {
            current->next = new Node();
            current = current->next;   
        }
        current->next = first;     
        
        head = first;   
        tail = first; =
        start_time = chrono::steady_clock::now();  
    }

    void produce(int item, int producer_id) {
        auto request_lock_time = chrono::steady_clock::now();  
        
        // Acquiring ticket lock to ensure fair synchronization
        ticket_lock_producer.lock();
        
        unique_lock<mutex> lock(mutex_producer);                  

        // Wait until the node at head is free to be filled
        cv_not_full.wait(lock, [this] { return !head->filled; });

        auto acquired_lock_time = chrono::steady_clock::now();  
        

        auto wait_duration = acquired_lock_time - request_lock_time;


        head->data = item;
        Node* temp = head->next;
        head->filled = true;
        head = temp; 

 
        auto now = chrono::steady_clock::now();
        long long timestamp = chrono::duration_cast<chrono::microseconds>(now - start_time).count();

        double waited_ms = chrono::duration_cast<chrono::duration<double, milli>>(wait_duration).count();

        // Logging
        logEvent("[" + to_string(timestamp) + "us] Producer "+ to_string(producer_id)+" waited for "+to_string(waited_ms)+"ms and produced: "+to_string(item));

        // Releasing the producer lock 
        lock.unlock();
        // Notifying one of the waiting consumer threads
        cv_not_empty.notify_one();

        ticket_lock_producer.unlock();

        auto end = chrono::steady_clock::now();

        lock_guard<mutex> stats_lock(prod_stat_mutex);
        total_produce_time += ((end - request_lock_time));
    }

    int consume(int consumer_id) {
        auto request_lock_time = chrono::steady_clock::now();

        // First consumer acquires lock to ensure synchronization
        unique_lock<mutex> lock(mutex_consumer);

        cv_not_empty.wait(lock, [this] { return tail->filled; });
        auto acquired_lock_time = chrono::steady_clock::now();
.
        auto wait_duration = acquired_lock_time - request_lock_time;

        // Consuming the current data
        int item = tail->data;
        tail->filled = false;
        
        auto now = chrono::steady_clock::now();
        long long timestamp = chrono::duration_cast<chrono::microseconds>(now - start_time).count();

        double waited_ms = chrono::duration_cast<chrono::duration<double, milli>>(wait_duration).count();

        // Logging
        logEvent("[" + to_string(timestamp) + "us] Consumer " + to_string(consumer_id)+" waited for "+to_string(waited_ms)+"ms and consumed: "+to_string(item));

        Node* temp = tail;
        tail = tail->next; 

        // Unlocking the buffer so other consumers can proceed
        lock.unlock();
        // Notifying producers that space is available
        cv_not_full.notify_one();

        auto end = chrono::steady_clock::now();
        lock_guard<mutex> stats_lock(cons_stat_mutex);
        total_consume_time += (end - request_lock_time);

        return item;
    }

    vector<double> Stats() {
        vector<double> time_stat;
        time_stat.push_back(total_produce_time.count());
        time_stat.push_back(total_consume_time.count());
        return time_stat;
    }

private:
    static mutex log_mutex;

    static void logEvent(const string& event) {
   
        lock_guard<mutex> lock(log_mutex);
        ofstream logFile("FiniteBufferLogger.txt", ios::app);
        logFile << event << endl;
    }
};

struct LogEvent {
    long long timestamp;        
    string type; // Producer or Consumer entry
    int id;     // ID of the producer or consumer thread
    int value; 
};

// Visualizer
class Visualizer {
    sf::View view;      
    float scrollOffset = 0.0f;  
private:
    vector<LogEvent> events;  

    void parseLogs() {
        ifstream in("FiniteBufferLogger.txt");
        string line;

        while (getline(in, line)) {
            LogEvent e;

            size_t ts_start = line.find('[');
            size_t ts_end = line.find("us]");

            if (ts_start == string::npos || ts_end == string::npos) continue;

            string ts_str = line.substr(ts_start + 1, ts_end - ts_start - 1);
            e.timestamp = stoll(ts_str);    
            if (line.find("Producer") != string::npos) {
                e.type = "Producer";
                sscanf(line.c_str(), "[%*lldus] Producer %d produced: %d", &e.id, &e.value);
            } else if (line.find("Consumer") != string::npos) {
                e.type = "Consumer";
                sscanf(line.c_str(), "[%*lldus] Consumer %d consumed: %d", &e.id, &e.value);
            }
            events.push_back(e);
        }
    }

public:
void run() {
    parseLogs();

    // Constants for visualizing nodes
    const int NODE_RADIUS = 25;
    const int NODE_SPACING = 50;
    const int WINDOW_WIDTH = 1400;
    const int WINDOW_HEIGHT = 600;

    // Creating the window for rendering
    sf::RenderWindow window(sf::VideoMode(WINDOW_WIDTH, WINDOW_HEIGHT), "Finite Buffer Producer-Consumer Visualisation");
    sf::Font font;
    font.loadFromFile("arial.ttf");

    vector<pair<sf::CircleShape, sf::Text>> nodes;

    // FPS Display Setup
    sf::Clock fpsClock;
    int frameCount = 0;
    float elapsedTime = 0.0f;
    sf::Text fpsText;
    fpsText.setFont(font);
    fpsText.setCharacterSize(14);
    fpsText.setFillColor(sf::Color::White);
    fpsText.setPosition(10, 10);

    if (events.empty()) return;  
    long long startTime = events[0].timestamp; 
    const float TIME_SCALE = 0.0003f; 

    sf::Clock globalClock;
    size_t current = 0;  

    view = window.getDefaultView();     

    // This loop will run as long as window is open
    while (window.isOpen()) {
        sf::Event event;
        while (window.pollEvent(event)) {
            if (event.type == sf::Event::Closed)
                window.close();     
            else if (event.type == sf::Event::MouseWheelScrolled)
                view.move(0, -event.mouseWheelScroll.delta * 30);      
        }

        window.clear(sf::Color(30, 30, 30));
        window.setView(view);

        float x = 50, y = 100; 
        const float max_x = 1000;    

        for (size_t i = 0; i < nodes.size(); ++i) {
            auto& [circle, text] = nodes[i];
            if (x > max_x) {
                x = 50;   
            }
            circle.setPosition(x, y);
            text.setPosition(x + 5, y + 5);
            x += NODE_RADIUS * 2 + NODE_SPACING;   
        }

        for (size_t i = 1; i < nodes.size(); ++i) {
            sf::Vertex line[] = {
                sf::Vertex(nodes[i - 1].first.getPosition() + sf::Vector2f(NODE_RADIUS, NODE_RADIUS), sf::Color::White),
                sf::Vertex(nodes[i].first.getPosition() + sf::Vector2f(NODE_RADIUS, NODE_RADIUS), sf::Color::White)
            };
            window.draw(line, 2, sf::Lines);
        }

        float currentTime = globalClock.getElapsedTime().asSeconds();
        while (current < events.size() && (events[current].timestamp - startTime) * TIME_SCALE <= currentTime) {
            auto& e = events[current];
            if (e.type == "Producer") {
                sf::CircleShape node(NODE_RADIUS);
                node.setFillColor(sf::Color::Blue);
                node.setOutlineThickness(2);
                node.setOutlineColor(sf::Color::White);

                sf::Text txt;
                txt.setFont(font);
                txt.setString(to_string(e.value));
                txt.setCharacterSize(16);
                txt.setFillColor(sf::Color::White);

                nodes.push_back({ node, txt });
            } 
            else if (e.type == "Consumer") {
                for (auto& [circle, text] : nodes) {
                    if (text.getString() == to_string(e.value)) {
                        circle.setFillColor(sf::Color::Red);
                        text.setString("");
                        break;
                    }
                }
            }
            current++;   
        }

        // FPS update
        frameCount++;
        elapsedTime += fpsClock.restart().asSeconds();
        if (elapsedTime >= 1.0f) {
            fpsText.setString("FPS: " + to_string(frameCount));
            frameCount = 0;
            elapsedTime = 0;
        }

        // Draw everything
        for (auto& [circle, text] : nodes) {
            window.draw(circle);
            window.draw(text);
        }

        window.draw(fpsText);   // Draw the FPS text
        window.display();   // Display the window content
    }
}
};

mutex LinkedListBuffer::log_mutex;

// Threads information by defualt
const int NUM_PRODUCERS = 5;
const int NUM_CONSUMERS = 3;
const int ITEMS_PER_PRODUCER = 30;
const int ITEMS_PER_CONSUMER = 50;

LinkedListBuffer buffer;   

void producer(int id) {
    for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
        int item = id * 1000 + i;   // Unique item based on producer ID
        this_thread::sleep_for(chrono::milliseconds(10));   // Simulating the work done by producer
        buffer.produce(item, id);
    }
}

void consumer(int id) {
    for (int i = 0; i < ITEMS_PER_CONSUMER; ++i) {
        buffer.consume(id);  
        this_thread::sleep_for(chrono::milliseconds(18)); // Simulate the work done by consumer
    }
}

struct LogEntry {
    long long timestamp;
    bool is_produce;
    double wait_time_ms;
};

// Driver code
int main() {

    ofstream("FiniteBufferLogger.txt") << "";

    vector<thread> threads;
    auto start_time = chrono::steady_clock::now(); 
    
    for (int i = 0; i < NUM_PRODUCERS; ++i)
        threads.emplace_back(producer, i + 1);

    for (int i = 0; i < NUM_CONSUMERS; ++i)
        threads.emplace_back(consumer, i + 1);
    for (auto& t : threads)
        t.join();

    auto end_time = chrono::steady_clock::now();

    vector<double> stat = buffer.Stats();  

    ifstream log("FiniteBufferLogger.txt");
    string line;
    vector<LogEntry> entries;

    // total_producer_wait: Total time producers spent waiting to acquire the lock 
    // total_consumer_wait: Total time consumers spent waiting for an item to become available and acquire lock
    int total_produced = 0, total_consumed = 0;
    double total_producer_wait = 0.0, total_consumer_wait = 0.0;
    double max_producer_wait = 0.0, max_consumer_wait = 0.0;

    unordered_map<int, double> producer_wait_sum;
    unordered_map<int, int> producer_wait_count;
    unordered_map<int, double> producer_wait_max;

    while (getline(log, line)) {
        size_t ts_start = line.find('[');
        size_t ts_end = line.find("us]");
        if (ts_start == string::npos || ts_end == string::npos) continue;

        long long timestamp = stoll(line.substr(ts_start + 1, ts_end - ts_start - 1));

        bool is_produce = (line.find("produced") != string::npos);

        // Find wait time
        double wait_ms = 0.0;
        size_t wait_pos = line.find("Waited:");
        if (wait_pos != string::npos) {
            stringstream ss(line.substr(wait_pos + 8));
            ss >> wait_ms;
        }

        if (is_produce) {
            total_produced++;
            total_producer_wait += wait_ms;
            if (wait_ms > max_producer_wait) max_producer_wait = wait_ms;
            
            size_t pid_pos = line.find("Producer ");
            if (pid_pos != string::npos) {
                int pid = stoi(line.substr(pid_pos + 9));
                producer_wait_sum[pid] += wait_ms;
                producer_wait_count[pid]++;
                producer_wait_max[pid] = max(producer_wait_max[pid], wait_ms);
            }
        } else {
            total_consumed++;
            total_consumer_wait += wait_ms;
            if (wait_ms > max_consumer_wait) max_consumer_wait = wait_ms;
        }
    }

    // Buffer size remain fixed
    int buffer_count = 10;
    int peak_buffer = 10;
    // Total runtime in seconds
    double total_runtime_sec = chrono::duration_cast<chrono::duration<double>>(end_time - start_time).count();

    // Displaying stats
    cout << fixed << setprecision(3);
    cout << "\nLOG ANALYSIS REPORT\n";
    cout << "Total Items Produced       : " << total_produced << "\n";
    cout << "Total Items Consumed       : " << total_consumed << "\n";
    cout << "Final Buffer Size          : " << peak_buffer << "\n";
    cout << "Peak Buffer Size (Nodes)   : " << peak_buffer << "\n";

    cout << "\nRuntime\n";
    cout << "Total Runtime              : " << total_runtime_sec << " seconds\n";
    cout << "Total Produce Time (just to produce in buffer including lock acquiring time and writing time)        : " << stat[0] << " seconds\n";
    cout << "Total Consume Time (just to consume from buffer including lock acquiring time and reading time)        : " << stat[1] << " seconds\n";

    cout << "\nProducer Stats\n";
    cout << "Total Wait Time            : " << total_producer_wait << " ms\n";
    cout << "Average Wait Time          : " << (total_produced ? total_producer_wait / total_produced : 0) << " ms\n";
    cout << "Maximum Wait Time          : " << max_producer_wait << " ms\n";

    cout << "\nConsumer Stats\n";
    cout << "Total Wait Time            : " << total_consumer_wait << " ms\n";
    cout << "Average Wait Time          : " << (total_consumed ? total_consumer_wait / total_consumed : 0) << " ms\n";
    cout << "Maximum Wait Time          : " << max_consumer_wait << " ms\n";
    
    // Producer Fairness
    cout << "\nProducer Fairness (by Avg Wait Time)\n";
    for (const auto& [pid, count] : producer_wait_count) {
        double avg_wait = producer_wait_sum[pid] / count;
        double max_wait = producer_wait_max[pid];
    
        cout << "Producer " << pid
             << " | Produced: " << count
             << " | Avg Wait Time: " << avg_wait << " ms"
             << " | Max Wait Time: " << max_wait << " ms"<<endl;
    }
    

    Visualizer vis;
    vis.run();  // Running the visualizer.

    return 0;
}