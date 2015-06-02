#include <fstream>
#include <sstream>
#include <iostream>
#include <assert.h>
#include <ctime>
#include <cmath>
#include <glog/logging.h>
#include <boost/chrono/thread_clock.hpp>

#include "thread.h"

extern map<string, Net<float>* > nets;

#define DEBUG 0

void SERVICE_fwd(float *in, int in_size, float *out, int out_size, Net<float>* net)
{
  float loss;
  vector<Blob<float>* > in_blobs = net->input_blobs();

  in_blobs[0]->set_cpu_data(in);
  vector<Blob<float>* > out_blobs = net->ForwardPrefilled(&loss);
  memcpy(out, out_blobs[0]->cpu_data(), sizeof(float));

  if(out_size != out_blobs[0]->count())
    LOG(FATAL) << "out_size =! out_blobs[0]->count())";
  else
    memcpy(out, out_blobs[0]->cpu_data(), out_size*sizeof(float));
}

pthread_t request_thread_init(int sock)
{
  // Prepare to create a new pthread
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 1024*1024);

  // Create a new thread starting with the function request_handler
  pthread_t tid;
  if(pthread_create(&tid, &attr, request_handler, (void *)sock) != 0)
    LOG(ERROR) << "Failed to create a request handler thread.\n";

  return tid;
}

void* request_handler(void* sock)
{
  int socknum = (int)sock;

  // 1. Client sends the application type
  // 2. Client sends the size of incoming data
  // 3. Client sends data

  char req_name[MAX_REQ_SIZE];
  SOCKET_receive(socknum, (char*)&req_name, MAX_REQ_SIZE, DEBUG);  
  map<string, Net<float>* >::iterator it = nets.find(req_name);
  if(it == nets.end()) {
    LOG(ERROR) << "Task " << req_name << " not found.";
    return;
  }
  else
    LOG(INFO) << "Task " << req_name << " forward pass.";

  // receive the input data length (in float)
  int sock_elts = SOCKET_rxsize(socknum);
  if(sock_elts < 0){
    LOG(ERROR) << "Error num incoming elts.";
    exit(1);
  }

  // reshape input dims if incoming data != current net config
  LOG(INFO) << "Elements received on socket " << sock_elts << endl;

  reshape(nets[req_name], sock_elts);

  int in_elts = nets[req_name]->input_blobs()[0]->count();
  int out_elts = nets[req_name]->output_blobs()[0]->count();
  float *in = (float*) malloc(in_elts * sizeof(float));
  float *out = (float*) malloc(out_elts * sizeof(float));

  // Main loop of the thread, following this order
  // 1. Receive input feature (has to be in the size of sock_elts)
  // 2. Do forward pass
  // 3. Send back the result
  // 4. Repeat 1-3

  // Warmup: used to move the network to the device for the first time
  // In all subsequent forward passes, the trained model resides on the
  // device (GPU)
  bool warmup = true;

  while(1) {
    LOG(INFO) << "Reading from socket.";
    int rcvd = SOCKET_receive(socknum, (char*) in, in_elts*sizeof(float), DEBUG);

    if(rcvd == 0) break; // Client closed the socket

    if(warmup) {
      float loss;
      vector<Blob<float>* > in_blobs = nets[req_name]->input_blobs();
      in_blobs[0]->set_cpu_data(in);
      vector<Blob<float>* > out_blobs;
      out_blobs = nets[req_name]->ForwardPrefilled(&loss);
      warmup = false;
    }

    LOG(INFO) << "Executing forward pass.";
    SERVICE_fwd(in, in_elts, out, out_elts, nets[req_name]);

    LOG(INFO) << "Writing to socket.";
    SOCKET_send(socknum, (char*) out, out_elts*sizeof(float), DEBUG);
  }

  // Exit the thread
  LOG(INFO) << "Socket closed by the client.";

  free(in);
  free(out);

  return;
}