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
#include <climits>
#include <SFML/Graphics.hpp>
#include <SFML/System.hpp>
#include <fstream>
#include <sstream>
#include <atomic>
#include <bits/stdc++.h>
using namespace std;

// -------------------- Node Definition --------------------
// Each node holds one item and a flag to indicate if it's filled.
// Nodes are linked dynamically to form an unbounded buffer.
struct Node {
    int data;
    bool filled;
    Node* next;
    Node() : data(0), filled(false), next(nullptr) {}
};

// -------------------- Ticket Lock for Producer Fairness --------------------
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
    Node* head; // Producer writes here
    Node* tail; // Consumer reads from here

    TicketLock ticket_lock_producer;       // Mutex for synchronizing producers access to the buffer
    mutex mutex_consumer;       // Mutex for synchronizing consumers access to the buffer
    condition_variable cv_not_empty;        // Condition variable used by consumers to wait until an item is available

    // Mutex to protect access to performance statistics
    mutex prod_stat_mutex;       
    mutex cons_stat_mutex;

    chrono::duration<double> total_produce_time{};      // Cumulative time spent by all producers (from attempting to acquire lock to releasing it)
    chrono::duration<double> total_consume_time{};      // Cumulative time spent by all consumers (from attempting to acquire lock to releasing it)
        
    chrono::steady_clock::time_point start_time;        // Timestamp marking the start of buffer activity


public:
    // Constructor: Initializes the infinite buffer with a dummy node.
    // The dummy node simplifies edge-case handling during insertions and deletions by ensuring that head and tail are always valid pointers.
    // The global start time of the buffer operation is also recorded here.
    LinkedListBuffer() {
        head = new Node();  // Initial dummy node
        tail = head;        // Both producer and consumer start here
        start_time = chrono::steady_clock::now();       // Mark the beginning of simulation
    }

    // Inserts an item into the infinite buffer.
    // Measures and logs the time spent waiting to acquire the lock.
    // Dynamically extends the buffer by allocating a new node after each insertion.
    void produce(int item, int producer_id) {

        // Mark the time when the producer starts trying to acquire the lock.
        auto request_lock_time = chrono::steady_clock::now();

        // Acquiring the producer lock to ensure mutual exclusion while modifying the buffer
        ticket_lock_producer.lock();
        auto acquired_lock_time = chrono::steady_clock::now();
        
        // Calculating how long the thread waited for the lock.
        auto wait_duration = acquired_lock_time - request_lock_time;
        
        // Store the produced item in the current node and mark it as filled.
        head->data = item;
        // Dynamically allocate a new node and link it to the current node.
        // This ensures the buffer can grow indefinitely as new items are produced.
        Node* new_node = new Node();
        head->next = new_node;
        head->filled = true;
        head = new_node;
        
        // Generating a timestamp relative to the start of buffer operation
        auto now = chrono::steady_clock::now();
        long long timestamp = chrono::duration_cast<chrono::microseconds>(now - start_time).count();
        
        // Converting the wait time into milliseconds for logging
        double waited_ms = chrono::duration_cast<chrono::duration<double, milli>>(wait_duration).count();

        // Logging the producer event
        logEvent("[" + to_string(timestamp) + "us] Producer " + to_string(producer_id) + " produced: " + to_string(item) + " | Waited: " + to_string(waited_ms) + "ms");
        
        // Releasing the producer lock so that other producers can enter the critical section.
        ticket_lock_producer.unlock();
        // Notifying one of the waiting consumer threads that a new item is available in the buffer
        cv_not_empty.notify_one();
        
        // Marking the time when production ends (after releasing the lock and notifying)
        auto end = chrono::steady_clock::now();

        // Updating cumulative production time (including wait + actual work),
        // ensuring thread-safe access to statistics using stat mutex.
        lock_guard<mutex> stats_lock(prod_stat_mutex);
        total_produce_time += ((end - request_lock_time));
    }

    // Removes an item from the infinite buffer.
    // Measures and logs the time spent waiting to acquire the lock and for data availability.
    // Frees memory by deleting consumed nodes to prevent memory buildup
    int consume(int consumer_id) {
        // consumer starts trying to acquire the lock
        auto request_lock_time = chrono::steady_clock::now();

        // Acquiring the consumer lock to ensure mutual exclusion while reading from the buffer.
        unique_lock<mutex> lock(mutex_consumer);

        // Blocking the consumer thread until an item is available (tail->filled == true).
        // This avoids busy waiting and efficiently synchronizes with the producer.
        cv_not_empty.wait(lock, [&]() {
            return tail->filled;
        });
        auto acquired_lock_time = chrono::steady_clock::now();
        
        // Measuring how long the thread waited (for the lock and data availability).
        auto wait_duration = acquired_lock_time - request_lock_time;
        
        // Retrieve the item from the buffer and mark the node as empty
        int item = tail->data;
        tail->filled = false;
        
        // Recording the timestamp of the consume event, relative to the global start time
        auto now = chrono::steady_clock::now();
        long long timestamp = chrono::duration_cast<chrono::microseconds>(now - start_time).count();
        
        // Converting the wait time to milliseconds for better logs
        double waited_ms = chrono::duration_cast<chrono::duration<double, milli>>(wait_duration).count();

        // Logging the consumer event
        logEvent("[" + to_string(timestamp) + "us] Consumer " + to_string(consumer_id) + " consumed: " + to_string(item) + " | Waited: " + to_string(waited_ms) + "ms");
    
        // Freeing up the memory dynamically
        Node* temp = tail;
        tail = tail->next;
        delete temp;
        
        // Unlocking the buffer so other consumers can proceed
        lock.unlock();
        
        // Update cumulative consumption time using a thread-safe mutex
        auto end = chrono::steady_clock::now();
        lock_guard<mutex> stats_lock(cons_stat_mutex);
        total_consume_time += (end - request_lock_time);
    
        return item;
    }

    // Returns a vector containing time-related statistics:
    // [0] = total time spent in produce() by all producers (in seconds)
    // [1] = total time spent in consume() by all consumers (in seconds)
    vector<double> Stats() {
        vector<double> time_stat;
        time_stat.push_back(total_produce_time.count());
        time_stat.push_back(total_consume_time.count());
        return time_stat;
    }

private:
    // Static mutex to ensure synchronized logging across all threads.
    // This prevents multiple threads from writing to the log file at the same time.
    static mutex log_mutex;

    static void logEvent(const string& event) {
        lock_guard<mutex> lock(log_mutex);      // Ensures exclusive access to the log file
        ofstream logFile("InfiniteBufferLogger.txt", ios::app);       // Opened the log file in append mode so existing content is preserved
        logFile << event << endl;
    }
};

// Struct to hold parsed information from each log entry.
// This structure is used during post-simulation analysis
struct LogEvent {
    long long timestamp;        // Time of the event in microseconds since buffer start
    string type; // "Producer" or "Consumer"  — identifies the source of the event
    int id;     // ID of the producer or consumer thread responsible for the event
    int value;  // The item produced or consumed
};

// Visualizer class handles parsing log events and managing view state for graphical display of the buffer operations timeline
class Visualizer {
    sf::View view;      // SFML view for scrollable/zoomable rendering
    float scrollOffset = 0.0f;  // Used to adjust vertical scroll in the visual display
private:
    vector<LogEvent> events;    // Stores structured log entries parsed from InfiniteBufferLogger.txt

    // Parses InfiniteBufferLogger.txt and populates the 'events' vector with LogEvent entries.
    // Each line is processed to extract timestamp, event type, thread ID, and item value
    void parseLogs() {
        ifstream in("InfiniteBufferLogger.txt");
        string line;

        while (getline(in, line)) {
            LogEvent e;

            // Extracting timestamp enclosed in [ ] and ending with "us]
            size_t ts_start = line.find('[');
            size_t ts_end = line.find("us]");

            if (ts_start == string::npos || ts_end == string::npos) continue;

            string ts_str = line.substr(ts_start + 1, ts_end - ts_start - 1);
            e.timestamp = stoll(ts_str);    // Converting string to long long

            if(line.find("$") != string::npos) continue;

            // Identifying whether the line represents a Producer or Consumer event
            if (line.find("Producer") != string::npos) {
                e.type = "Producer";
                // Extracting producer ID and item value
                sscanf(line.c_str(), "[%*lldus] Producer %d produced: %d", &e.id, &e.value);
            } else if (line.find("Consumer") != string::npos) {
                e.type = "Consumer";
                // Extracting Consumer ID and item value
                sscanf(line.c_str(), "[%*lldus] Consumer %d consumed: %d", &e.id, &e.value);
            }

            // Adding the parsed event to the list for later rendering
            events.push_back(e);
        }
    }

public:
void run() {
    // Exttacting events
    parseLogs();

    // Constants for visualizing nodes
    const int NODE_RADIUS = 25;
    const int NODE_SPACING = 50;    // Spacing between nodes horizontally
    const int WINDOW_WIDTH = 1400;
    const int WINDOW_HEIGHT = 600;

    // Creating the window for rendering
    sf::RenderWindow window(sf::VideoMode(WINDOW_WIDTH, WINDOW_HEIGHT), "Infinite Buffer Producer-Consumer Visualisation");
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

    // Time-based animation setup
    if (events.empty()) return;  // If there are no events, stop execution
    long long startTime = events[0].timestamp;  // Timestamp of the first event
    const float TIME_SCALE = 0.0002f;  // Scaling factor for animation speed (microseconds --> seconds)

    sf::Clock globalClock;  // Global clock to track time for events
    size_t current = 0;  // Index to track current event being processed

    view = window.getDefaultView();        // default view for the window

    // Main loop: runs as long as the window is open
    while (window.isOpen()) {
        sf::Event event;
        while (window.pollEvent(event)) {
            if (event.type == sf::Event::Closed)
                window.close();     // Closing the window if the close event is triggered
            else if (event.type == sf::Event::MouseWheelScrolled)
                view.move(0, -event.mouseWheelScroll.delta * 30);        // For scrolling
        }

        // Clear the window with a dark background color
        window.clear(sf::Color(30, 30, 30));
        window.setView(view);

        // Drawing nodes
        float x = 50, y = 100;  // Starting position for nodes
        const float max_x = WINDOW_WIDTH - 100;      // Max x position to prevent nodes from going out of the window

        for (size_t i = 0; i < nodes.size(); ++i) {
            auto& [circle, text] = nodes[i];
            if (x > max_x) {
                x = 50;     // If x goes out of window size then we reset it
                y += NODE_RADIUS * 2 + 30;  // also we come to same row
            }
            circle.setPosition(x, y);
            text.setPosition(x + 5, y + 5);
            x += NODE_RADIUS * 2 + NODE_SPACING;    // shifting x for next node
        }

        // Drawing lines connecting nodes
        for (size_t i = 1; i < nodes.size(); ++i) {
            sf::Vertex line[] = {
                sf::Vertex(nodes[i - 1].first.getPosition() + sf::Vector2f(NODE_RADIUS, NODE_RADIUS), sf::Color::White),
                sf::Vertex(nodes[i].first.getPosition() + sf::Vector2f(NODE_RADIUS, NODE_RADIUS), sf::Color::White)
            };
            window.draw(line, 2, sf::Lines);
        }

        // Animating events based on actual timestamp (scaled)
        float currentTime = globalClock.getElapsedTime().asSeconds();
        while (current < events.size() && (events[current].timestamp - startTime) * TIME_SCALE <= currentTime) {
            auto& e = events[current];
            // If event type is "Producer", create a new node (producer)
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
            // If event type is "Consumer", we mark a consumed node (change its color to red)
            else if (e.type == "Consumer") {
                for (auto& [circle, text] : nodes) {
                    if (text.getString() == to_string(e.value)) {
                        circle.setFillColor(sf::Color::Red);
                        text.setString(""); // fade consumed value
                        break;
                    }
                }
            }
            current++;    // Moving to the next event
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

// -------------------- Producer & Consumer Threads --------------------
const int NUM_PRODUCERS = 5;
const int NUM_CONSUMERS = 3;
const int ITEMS_PER_PRODUCER = 30;
const int ITEMS_PER_CONSUMER = 50;

LinkedListBuffer buffer;    // Shared buffer among all producers and consumers

void producer(int id) {
    for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
        int item = id * 1000 + i;   // Unique item based on producer ID
        this_thread::sleep_for(chrono::milliseconds(10));   // Simulate work
        buffer.produce(item, id);   // Add item to the buffer
    }
}

void consumer(int id) {
    for (int i = 0; i < ITEMS_PER_CONSUMER; ++i) {
        buffer.consume(id);  // Remove item from the buffer
        this_thread::sleep_for(chrono::milliseconds(18));  // Simulate processing time
    }
}

struct LogEntry {
    long long timestamp;
    bool is_produce;  // true = produced, false = consumed
    double wait_time_ms;    // Time spent waiting (for lock or buffer)
};


// -------------------- Main --------------------
int main() {
    ofstream("InfiniteBufferLogger.txt") << ""; // Clear old logs

    vector<thread> threads;

    auto start_time = chrono::steady_clock::now(); // Tracks total runtime

    // Launch all producer threads
    for (int i = 0; i < NUM_PRODUCERS; ++i)
        threads.emplace_back(producer, i + 1);

    // Launch all consumer threads
    for (int i = 0; i < NUM_CONSUMERS; ++i)
        threads.emplace_back(consumer, i + 1);

    // Wait for all threads to complete
    for (auto& t : threads)
        t.join();

    auto end_time = chrono::steady_clock::now();

    vector<double> stat = buffer.Stats();   // Getting produce/consume durations

    ifstream log("InfiniteBufferLogger.txt");
    string line;
    vector<LogEntry> entries;

    // Stats
    // total_producer_wait: Total time producers spent waiting to acquire the lock (here only wait time will be to acquire lock(producer mutex) as infinte buffer is there)
    // total_consumer_wait: Total time consumers spent waiting for an item to become available(non empty buffer) + acquire lock(consumer mutex)
    int total_produced = 0, total_consumed = 0;
    double total_producer_wait = 0.0, total_consumer_wait = 0.0;
    double max_producer_wait = 0.0, max_consumer_wait = 0.0;

    // For producer fairness analysis
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

        entries.push_back({timestamp, is_produce, wait_ms});

        // Aggregate for stats
        if (is_produce) {
            total_produced++;
            total_producer_wait += wait_ms;
            if (wait_ms > max_producer_wait) max_producer_wait = wait_ms;
        
            // Track which producer produced
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

    // Sort entries by timestamp
    sort(entries.begin(), entries.end(), [](const LogEntry& a, const LogEntry& b) {
        return a.timestamp < b.timestamp;
    });

    // Calculate peak buffer size over time
    int buffer_count = 0;
    int peak_buffer = 0;
    for (const auto& e : entries) {
        buffer_count += (e.is_produce ? 1 : -1);
        if (buffer_count > peak_buffer)
            peak_buffer = buffer_count;
    }

    // Total runtime in seconds
    double total_runtime_sec = chrono::duration_cast<chrono::duration<double>>(end_time - start_time).count();
    
    // Displaying stats
    cout << fixed << setprecision(3);
    cout << "\n===== LOG ANALYSIS REPORT =====\n";
    cout << "Total Items Produced       : " << total_produced << "\n";
    cout << "Total Items Consumed       : " << total_consumed << "\n";
    cout << "Final Buffer Size          : " << (total_produced - total_consumed) << "\n";
    cout << "Peak Buffer Size (Nodes)   : " << peak_buffer << "\n";

    cout << "\n--- Runtime ---\n";
    cout << "Total Runtime              : " << total_runtime_sec << " seconds\n";
    cout << "Total Produce Time (just to produce in buffer including lock acquiring time and writing time) : " << stat[0] << " seconds\n";
    cout << "Total Consume Time (just to consume from buffer including lock acquiring time and reading time): " << stat[1] << " seconds\n";

    cout << "\n--- Producer Stats ---\n";
    cout << "Total Wait Time            : " << total_producer_wait << " ms\n";
    cout << "Average Wait Time          : " << (total_produced ? total_producer_wait / total_produced : 0) << " ms\n";
    cout << "Maximum Wait Time          : " << max_producer_wait << " ms\n";

    cout << "\n--- Consumer Stats ---\n";
    cout << "Total Wait Time            : " << total_consumer_wait << " ms\n";
    cout << "Average Wait Time          : " << (total_consumed ? total_consumer_wait / total_consumed : 0) << " ms\n";
    cout << "Maximum Wait Time          : " << max_consumer_wait << " ms\n";

    // Producer Fairness
    cout << "\n--- Producer Fairness (by Avg Wait Time) ---\n";
    for (const auto& [pid, count] : producer_wait_count) {
        double avg_wait = producer_wait_sum[pid] / count;
        double max_wait = producer_wait_max[pid];
    
        cout << "Producer " << pid
             << " | Produced: " << count
             << " | Avg Wait Time: " << avg_wait << " ms"
             << " | Max Wait Time: " << max_wait << " ms"<<endl;
    }

    cout << "\n=================================\n";
    cout << "\nAll tasks done. Check InfiniteBufferLogger.txt for logs.\n";

    Visualizer vis;
    vis.run();  // Replaying activity using SFML graphics

    return 0;
}
