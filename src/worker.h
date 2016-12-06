#include <iostream>
#include <mutex>
#include <functional>
#include <time.h>
#include <unistd.h>
#include <memory>
#include <pmmintrin.h>
#include <immintrin.h>
#include "sparsehash_memory/sparsepp.h"

#include "sparsehash_cpu/internal/sparseconfig.h"
#include "sparsehash_cpu/type_traits.h"
#include "sparsehash_cpu/sparsetable"
#include "hash_test_interface.h"

#include "./io/load_data.cc"
#include "threadpool/thread_pool.h"
#include "ps.h"

#include <netdb.h>  
#include <net/if.h>  
#include <arpa/inet.h>  
#include <sys/ioctl.h>  
#include <sys/types.h>  
#include <sys/socket.h>  

#define IP_SIZE     16

namespace dmlc{
class Worker : public ps::App{
    public:
        Worker(const char *train_file, const char *test_file) : 
                train_file_path(train_file), test_file_path(test_file){ 
            char ip[IP_SIZE];
            const char *test_eth = "eth0"; 
            get_local_ip(test_eth, ip);
            std::cout<<"worker ip "<<ip<<std::endl;
        }
        ~Worker(){
            delete train_data;
            //delete test_data;
        } 
 
    void get_local_ip(const char *eth_inf, char *ip)  {  
        int sd;  
        struct sockaddr_in sin;  
        struct ifreq ifr;  
        sd = socket(AF_INET, SOCK_DGRAM, 0);  
        if (-1 == sd){  
            std::cout<<"socket error: "<<strerror(errno)<<std::endl;  
            return;        
        }  
        strncpy(ifr.ifr_name, eth_inf, IFNAMSIZ);  
        ifr.ifr_name[IFNAMSIZ - 1] = 0;  
        if (ioctl(sd, SIOCGIFADDR, &ifr) < 0){  
            std::cout<<"ioctl error: "<<strerror(errno)<<std::endl;  
            close(sd);  
            return;  
        }  
        memcpy(&sin, &ifr.ifr_addr, sizeof(sin));  
        snprintf(ip, IP_SIZE, "%s", inet_ntoa(sin.sin_addr));  
        close(sd);  
    }  

        virtual void ProcessRequest(ps::Message* request){
            //do nothing.
        }

        float sigmoid(float x){
            if(x < -30) return 1e-6;
            else if(x > 30) return 1.0;
            else{
                double ex = pow(2.718281828, x);
                return ex / (1.0 + ex);
            }
        }

	    virtual bool Run(){
	        Process();
	    }

        inline void filter_zero_element(std::vector<float>& gradient, std::vector<ps::Key>& nonzero_index, std::vector<float>& nonzero_gradient){
            for(int i = 0; i < init_index.size(); i++){
                int idx = init_index[i];
                float g = gradient[idx];
                if(g != 0.0){
                    nonzero_index.push_back(idx);
                    nonzero_gradient.push_back(g);
                }
            }
        }

        timespec time_diff(timespec start, timespec end){
            timespec tmp;
            tmp.tv_sec =  end.tv_sec - start.tv_sec;
            tmp.tv_nsec = end.tv_nsec - start.tv_nsec;
            return tmp;
        }

        void calculate_one_gradient(int start, int end){
            timespec all_start, all_end, all_elapsed_time;
            clock_gettime(CLOCK_MONOTONIC, &all_start);

            size_t idx = 0; int value = 0; float pctr = 0;
            
            for(int row = start; row < end; ++row){
                int sample_size = train_data->fea_matrix[row].size();
                auto keys = std::make_shared<std::vector<ps::Key>> ();
                for(int j = 0; j < sample_size; ++j){//for one instance
                    idx = train_data->fea_matrix[row][j].fid;
                    (*keys).push_back(idx);
                }
                std::sort((*keys).begin(), (*keys).end());
                std::vector<float> w;

                timespec pull_start_time, pull_end_time, pull_elapsed_time;
                clock_gettime(CLOCK_MONOTONIC, &pull_start_time);
                kv_.Wait(kv_.ZPull(keys, &w));
                clock_gettime(CLOCK_MONOTONIC, &pull_end_time);
                pull_elapsed_time = time_diff(pull_start_time, pull_end_time);

                float wx = bias;
                int keys_size = (*keys).size();
                for(int j = 0; j < keys_size; ++j){
                    wx += w[j];
                }
                pctr = sigmoid(wx);
                float delta = pctr - train_data->label[row];
                auto gradient = std::make_shared<std::vector<float> > ();
                for(int j = 0; j < keys_size; j++){
                    (*gradient).push_back(delta);
                }

                timespec push_start_time, push_end_time, push_elapsed_time;
                clock_gettime(CLOCK_MONOTONIC, &push_start_time);
                kv_.Wait(kv_.ZPush(keys, gradient));//put gradient to servers;
                clock_gettime(CLOCK_MONOTONIC, &push_end_time);
                push_elapsed_time = time_diff(push_start_time, push_end_time);

                clock_gettime(CLOCK_MONOTONIC, &all_end);
                all_elapsed_time = time_diff(all_start, all_end);

                all_time += all_elapsed_time.tv_sec * 1e9 + all_elapsed_time.tv_nsec; 
                all_pull_time += pull_elapsed_time.tv_sec * 1e9 + pull_elapsed_time.tv_nsec;
                all_push_time += push_elapsed_time.tv_sec * 1e9 + push_elapsed_time.tv_nsec;
                send_key_numbers += keys_size;
            }
        }

        void call_back_calculate_batch_gradient(int start, int end){
            timespec all_start, all_end, all_elapsed_time;
            clock_gettime(CLOCK_MONOTONIC, &all_start);

            auto keys = std::make_shared<std::vector<ps::Key>> ();
            for(int row = start; row < end; ++row){
                int sample_size = train_data->fea_matrix[row].size();
                for(int j = 0; j < sample_size; ++j){//for one instance
                    size_t idx = train_data->fea_matrix[row][j].fid;
                    (*keys).push_back(idx);
                }
            }
            std::sort((*keys).begin(), (*keys).end());
            std::vector<ps::Key>::iterator iter_keys;
            iter_keys = unique((*keys).begin(), (*keys).end());
            (*keys).erase(iter_keys, (*keys).end());
            auto w = new std::vector<float>();

            ps::SyncOpts callback_pull;
            callback_pull.callback = [this, keys, w, start, end](){
                GOOGLE_NAMESPACE::dense_hash_map<size_t, float> weight;
                weight.set_empty_key(-1);
                size_t keys_size = (*keys).size();
                for(int i = 0; i < keys_size; i++){
                    weight[(*keys)[i]] = (*w)[i];
                }
                GOOGLE_NAMESPACE::dense_hash_map<size_t, float> gradient;
                gradient.set_empty_key(-1);
                for(int row = start; row < end; ++row){
                    float wx = bias;
                    int sample_size = train_data->fea_matrix[row].size();
                    for(int j = 0; j < sample_size; ++j){
                        size_t idx = train_data->fea_matrix[row][j].fid;
                        wx += weight[idx];
                    }
                    float pctr = sigmoid(wx);
                    float delta = pctr - train_data->label[row];
                    for(int j = 0; j < keys_size; j++){
                        gradient[(*keys)[j]] += delta;
                    }
                }
                auto push_keys = std::make_shared<std::vector<ps::Key> >();
                auto push_gradient = std::make_shared<std::vector<float> >();
                std::map<size_t, float> ordered(gradient.begin(), gradient.end());
                for(auto& iter : ordered){
                    (*push_keys).push_back(iter.first);
                    (*push_gradient).push_back(gradient[iter.first]);
                }

                ps::SyncOpts callback_push;
                callback_push.callback = [this](){
                    --num_batch_fly;
                };
                kv_.ZPush(push_keys, push_gradient, callback_push);//put gradient to servers;
            };
            kv_.ZPull(keys, w, callback_pull);

            //clock_gettime(CLOCK_MONOTONIC, &all_end);
            //all_elapsed_time = time_diff(all_start, all_end);

            //all_time += all_elapsed_time.tv_sec * 1e9 + all_elapsed_time.tv_nsec;
            send_key_numbers += (*keys).size();
        }

        void calculate_batch_gradient(int start, int end){
            timespec all_start, all_end, all_elapsed_time;
            clock_gettime(CLOCK_MONOTONIC, &all_start);

            size_t idx = 0; int value = 0; float pctr = 0;
            auto keys = std::make_shared<std::vector<ps::Key>> ();
            for(int row = start; row < end; ++row){
                int sample_size = train_data->fea_matrix[row].size();
                for(int j = 0; j < sample_size; ++j){//for one instance
                    idx = train_data->fea_matrix[row][j].fid;
                    (*keys).push_back(idx);
                }
            }
            std::sort((*keys).begin(), (*keys).end());
            std::vector<ps::Key>::iterator iter_keys;
            iter_keys = unique((*keys).begin(), (*keys).end());
            (*keys).erase(iter_keys, (*keys).end());
            int keys_size = (*keys).size();
            //std::vector<float> w(keys_size);
            auto w = std::make_shared<std::vector<float>>();

            timespec pull_start_time, pull_end_time, pull_elapsed_time;
            clock_gettime(CLOCK_MONOTONIC, &pull_start_time);
            kv_.Wait(kv_.ZPull(keys, &(*w)));
            clock_gettime(CLOCK_MONOTONIC, &pull_end_time);
            pull_elapsed_time = time_diff(pull_start_time, pull_end_time);
            /*
            //std::unordered_map<size_t, float> weight(keys_size);
            //spp::sparse_hash_map<size_t, float> weight;
            GOOGLE_NAMESPACE::dense_hash_map<size_t, float> weight(keys_size);
            weight.set_empty_key(-1);
            for(int i = 0; i < keys_size; i++){
                weight[(*keys)[i]] = w[i];
            }
            //std::unordered_map<size_t, float> gradient(keys_size);
            //spp::sparse_hash_map<size_t, float> gradient;
            GOOGLE_NAMESPACE::dense_hash_map<size_t, float> gradient(keys_size);
            gradient.set_empty_key(-1);

            for(int row = start; row < end; ++row){
                float wx = bias;
                int sample_size = train_data->fea_matrix[row].size();
                for(int j = 0; j < sample_size; ++j){
                    idx = train_data->fea_matrix[row][j].fid;
                    wx += weight[idx];
                }
                pctr = sigmoid(wx);
                //float delta = pctr - train_data->label[row];
                float delta = pctr - 0.0;
                for(int j = 0; j < keys_size; j++){
                    gradient[(*keys)[j]] += delta;
                }
            }

            auto push_keys = std::make_shared<std::vector<ps::Key> > (keys_size);
            auto push_gradient = std::make_shared<std::vector<float> > (keys_size);
            std::map<size_t, float> ordered(gradient.begin(), gradient.end());
            for(auto& iter : ordered){
                (*push_keys).push_back(iter.first);
                (*push_gradient).push_back(gradient[iter.first]);
            }
            */
            //auto push_gradient = std::make_shared<std::vector<float> > (keys_size);
            //for(int i = 0; i < keys_size; i++) (*push_gradient).push_back(1000.0);
            timespec push_start_time, push_end_time, push_elapsed_time;
            clock_gettime(CLOCK_MONOTONIC, &push_start_time);
            //kv_.Wait(kv_.ZPush(push_keys, push_gradient));//put gradient to servers;
            kv_.Wait(kv_.ZPush(keys, w));//put gradient to servers;
            clock_gettime(CLOCK_MONOTONIC, &push_end_time);
            push_elapsed_time = time_diff(push_start_time, push_end_time);

            clock_gettime(CLOCK_MONOTONIC, &all_end);
            all_elapsed_time = time_diff(all_start, all_end);

            all_time += all_elapsed_time.tv_sec * 1e9 + all_elapsed_time.tv_nsec; 
            all_pull_time += pull_elapsed_time.tv_sec * 1e9 + pull_elapsed_time.tv_nsec;
            all_push_time += push_elapsed_time.tv_sec * 1e9 + push_elapsed_time.tv_nsec;
            send_key_numbers += keys_size;
        }

        void online_learning(int core_num){
            ThreadPool pool(core_num);
            train_data = new dml::LoadData(train_data_path);
            int batch = 0, start, end, thread_batch = batch_size / core_num;
            while(1){
                train_data->load_minibatch_data(batch_size);
                if(train_data->fea_matrix.size() < batch_size){
                    std::cout<<"read all"<<std::endl;
                    break;
                }
                calculate_gradient_thread_count = 0;

                for(int i = 0; i < core_num; ++i){
                    start = i * thread_batch;
                    end = (i + 1) * thread_batch;
                    pool.enqueue(std::bind(&Worker::calculate_batch_gradient, this, start, end));
                }//end for
                while(calculate_gradient_thread_count < core_num);
                if((batch + 1) % 20 == 0)std::cout<<"rank "<<rank<<" batch = "<<batch<<std::endl;
                ++batch;
            }//end while one epoch
        }

        void batch_learning(int core_num){
            train_data = new dml::LoadData(train_data_path);
            //train_data->load_all_data();
            //std::hash<std::string> h;
            int train_data_size = 4e4;
            train_data->fea_matrix.clear();
            std::srand(std::time(0));
            for(int i = 0; i < train_data_size; i++){
                if(i % 20000 == 0) std::cout<<" init train_data "<<i<<std::endl;
                train_data->sample.clear();
                for(int j = 0; j < 20000; j++){
                    train_data->keyval.fid = std::rand();
                    train_data->sample.push_back(train_data->keyval);
                }
                train_data->fea_matrix.push_back(train_data->sample);
            }
 
            /*
            for(int i = 0; i < train_data_size; i++){
                for(int j = 0; j < 2000; j++){
                    std::cout<<train_data->fea_matrix[i][j].fid<<" ";    
                } 
                std::cout<<std::endl;
            }
            */

            //std::cout<<"train_data size : "<<train_data->fea_matrix.size()<<std::endl;

            //core_num *= 2;
            ThreadPool pool(core_num);

            //batch_num = train_data->fea_matrix.size() / batch_size;
            batch_num = train_data_size / batch_size;
            std::cout<<"batch_num : "<<batch_num<<std::endl;
            timespec allstart, allend, allelapsed;
            for(int epoch = 0; epoch < epochs; ++epoch){
                size_t old_all_time = 0;
                size_t old_all_push_time = 0;
                size_t old_all_pull_time = 0;
                clock_gettime(CLOCK_MONOTONIC, &allstart);
                send_key_numbers = 0;
                for(int i = 0; i < batch_num; ++i){
                    /*
                    if((i + 1)%300 == 0){
                        std::cout<<"rank "<<rank<<" epoch "<<epoch<<" batch "<<i<<std::endl;
                        size_t all = (all_time - old_all_time)* 1.0 / (300 * core_num);
                        std::cout<<"rank "<<rank<<" all time avage: "<<(all_time - old_all_time)* 1.0 / (300 * core_num) <<std::endl;
                        std::cout<<"rank "<<rank<<" all push time avage: "<<(all_push_time - old_all_push_time) * 1.0 / (300 * core_num)<<std::endl;
                        std::cout<<"rank "<<rank<<" all pull time avage: "<<(all_pull_time - old_all_pull_time) * 1.0 / (300 * core_num)<<std::endl;
                        std::cout<<"rank "<<rank<<" process per second "<<batch_size * 1e9 / all<<" thread per second "<<batch_size * 1e9 / (all * core_num) <<std::endl; 
                        old_all_time = all_time;
                        old_all_push_time = all_push_time;
                        old_all_pull_time = all_pull_time;
                    }
                    */
                    int all_start = i * batch_size;
                    int thread_batch = batch_size / core_num;
                    int start, end;
                    for(int j = 0; j < core_num; ++j){
                        start = all_start + j * thread_batch;
                        end = all_start + (j + 1) * thread_batch;
                        pool.enqueue(std::bind(&Worker::calculate_batch_gradient, this, start, end));
                        //pool.enqueue(std::bind(&Worker::calculate_one_gradient, this, start, end));
                        /* 
                        while(num_batch_fly > core_num * 4) usleep(100);
                        pool.enqueue(std::bind(&Worker::call_back_calculate_batch_gradient, this, start, end));
                        //call_back_calculate_batch_gradient(start, end);
                        ++num_batch_fly;
                        */
                    }
                }//end all batch
                clock_gettime(CLOCK_MONOTONIC, &allend);
                allelapsed = time_diff(allstart, allend);
                std::cout<<"rank "<<rank<<" per process : "<<train_data->fea_matrix.size() * 1e9 * 1.0 / (allelapsed.tv_sec * 1e9 + allelapsed.tv_nsec)<<std::endl;
                //std::cout<<"rank "<<rank<<" all time avage: "<<all_time * 1.0 / (batch_num * core_num) <<std::endl;
                //std::cout<<"rank "<<rank<<" all push time avage: "<<all_push_time * 1.0 / (batch_num * core_num)<<std::endl;
                //std::cout<<"rank "<<rank<<" all pull time avage: "<<all_pull_time * 1.0 / (batch_num * core_num)<<std::endl;
                std::cout<<"rank "<<rank<<" send_key_number avage: "<<send_key_numbers * 1.0 / (batch_num * core_num)<<std::endl;
            }//end all epoch
        }//end batch_learning

        virtual void Process(){
            rank = ps::MyRank();
            snprintf(train_data_path, 1024, "%s-%05d", train_file_path, rank);

            core_num = std::thread::hardware_concurrency();

            if(is_online_learning == 1){
                online_learning(core_num);
            }
            else if(is_batch_learning == 1){
                batch_learning(core_num);
            }
            std::cout<<"train end......"<<std::endl;
            /*
            snprintf(test_data_path, 1024, "%s-%05d", test_file_path, rank);
            test_data = new dml::LoadData(test_data_path);
            test_data->load_all_data();
            predict(rank);
            std::cout<<"rank "<<rank<<" end!"<<std::endl;
            */
        }//end process

    public:
        int rank;
        int core_num;
        int batch_num;
        int call_back = 1;
        int batch_size = 800;
        int epochs = 5;
        int calculate_gradient_thread_count;
        int is_online_learning = 0;
        int is_batch_learning = 1;

        std::atomic_llong  num_batch_fly = {0};
        std::atomic_llong all_time = {0};
        std::atomic_llong all_push_time = {0};
        std::atomic_llong all_pull_time = {0};
        std::atomic_llong send_key_numbers = {0};
        
        std::mutex mutex;
        std::vector<ps::Key> init_index;
        dml::LoadData *train_data;
        dml::LoadData *test_data;
        const char *train_file_path;
        const char *test_file_path;
        char train_data_path[1024];
        char test_data_path[1024];
        float bias = 0.0;
        ps::KVWorker<float> kv_;
};//end class worker

}//end namespace dmlc 
