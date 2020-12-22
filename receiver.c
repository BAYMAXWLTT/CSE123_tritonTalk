#include "receiver.h"


void init_receiver(Receiver* receiver, int id) {
    // pthread_cond_init(&receiver->buffer_cv, NULL);
    // pthread_mutex_init(&receiver->buffer_mutex, NULL);
    
    receiver->recv_id = id;
    receiver->input_framelist_head = NULL;
    receiver->msg_buffer = calloc(glb_senders_array_length, sizeof(char*));
    receiver->prev_id = calloc(glb_senders_array_length, sizeof(int));
    receiver->sender_state = calloc(glb_receivers_array_length, sizeof(Send_state));

    // init message buffer for each sender
    for(int i = 0; i < glb_senders_array_length; i ++) {
        receiver->msg_buffer[i] = calloc(BUFSIZ, sizeof(char));
    }

    // init id field to be -1
    for(int i = 0; i < glb_senders_array_length; i ++) {
        receiver->prev_id[i] = -1;
    }

    // init message buffer
    for(int i = 0; i < glb_senders_array_length; i ++) {
        receiver->sender_state[i].NFE = 1;
        
        // init recvQ
        for(int j = 0; j < RWS; j ++) {
            receiver->sender_state[i].recvQ[j] = (recvQ_slot){NULL, 0};
        }
        
        for(int j = 0; j < MAX_BUFFER_LEN; j ++) {
            receiver->sender_state[i].buffer[j] = NULL;
        }
    }
    
}

int is_in_window_recv(Receiver* recv, Frame* inframe) {
    int id = inframe->id;
    int sender_id = inframe->src_id;
    int NFE = recv->sender_state[sender_id].NFE;

    //printf("current NFE: %d\n", NFE);
    if((NFE + RWS - 1) < SEQ_LENGTH && 
       (id < NFE || id >= (NFE + RWS))) {
        return 0;
    }

    if((NFE + RWS - 1) >= SEQ_LENGTH && 
       ((id >= (NFE + RWS) % SEQ_LENGTH) && id < NFE)) {
        return 0;
    } 

    return 1;
}

char* reassemble_msg(Frame** buffer, unsigned long* length) {
    char* res = calloc(sizeof(char), BUFSIZ);
    
    // find end and start
    int start = -1;
    int end = -1;
    for(int i = 0; i < MAX_BUFFER_LEN; i ++) {
        if(buffer[i] != NULL) {
            start = i;
            break;
        }
    }

    for(int i = 0; i < MAX_BUFFER_LEN; i ++) {
        if(buffer[i] != NULL) {
            if(buffer[i]->is_end == 1) {
                end = i;
                *length = buffer[i]->length;
            }
        }    
    }

    if(start == -1 || end == -1) return NULL;

    // check wrap around
    for(int i = end + 1; i < MAX_BUFFER_LEN; i ++) {
        if(buffer[i] != NULL) {
            start = i;
        }
    }

    // reassmble msg
    if(start <= end) {
        for(int i = start; i <= end; i ++) {
            if(buffer[i] == NULL) return NULL;
            char temp[FRAME_PAYLOAD_SIZE + 1];
            temp[FRAME_PAYLOAD_SIZE] = '\0';
            strncpy(temp, buffer[i]->data, FRAME_PAYLOAD_SIZE);
            strcat(res, temp);
        }
    } else {
        for(int i = start; i <= end + MAX_BUFFER_LEN; i ++) {
            if(buffer[i % MAX_BUFFER_LEN] == NULL) return NULL;
            char temp[FRAME_PAYLOAD_SIZE + 1];
            temp[FRAME_PAYLOAD_SIZE] = '\0';
            strncpy(temp, buffer[i % MAX_BUFFER_LEN]->data, FRAME_PAYLOAD_SIZE);
            strcat(res, temp);
        }
    }

    return res;
}

void handle_incoming_msgs(Receiver* receiver,
                          LLnode** outgoing_frames_head_ptr) {
    // TODO: Suggested steps for handling incoming frames
    //    1) Dequeue the Frame from the sender->input_framelist_head
    //    2) Convert the char * buffer to a Frame data type
    //    3) Check whether the frame is for this receiver
    //    4) Acknowledge that this frame was received

    int incoming_msgs_length = ll_get_length(receiver->input_framelist_head);
    
    while (incoming_msgs_length > 0) {
        // Pop a node off the front of the link list and update the count
        LLnode* ll_inmsg_node = ll_pop_node(&receiver->input_framelist_head);
        incoming_msgs_length = ll_get_length(receiver->input_framelist_head);

        // DUMMY CODE: Print the raw_char_buf
        // NOTE: You should not blindly print messages!
        //      Ask yourself: Is this message really for me?
        char* raw_char_buf = ll_inmsg_node->value;
        Frame* inframe = convert_char_to_frame(raw_char_buf);

        char temp[FRAME_PAYLOAD_SIZE + 1];
        strncpy(temp, inframe->data, FRAME_PAYLOAD_SIZE);
        temp[FRAME_PAYLOAD_SIZE] = '\0';
    
        if(is_corrupted(inframe)) {
            // free(ll_inmsg_node);
            // free(inframe);
            //printf("corrupted!\n");
            continue;
        }

        //printf("inframe data: %s\n", temp);
        // check if this frame is for me
        if(inframe->dst_id == receiver->recv_id) {
            // check if this frame is sent before
            //printf("src id %d\n", inframe->id);
            if((receiver->sender_state[inframe->src_id].recvQ[inframe->id % RWS].received == 1 && 
                receiver->sender_state[inframe->src_id].recvQ[inframe->id % RWS].frame->id == inframe->id) ||
                (!is_in_window_recv(receiver, inframe))) {
                
                // sent before, then ack again
                //printf("received before/ out of window %d\n", inframe->id);
                inframe->ack_id = receiver->sender_state[inframe->src_id].NFE - 1;
                inframe->dst_id = receiver->recv_id;
                //compute_crc(convert_frame_to_char(inframe), inframe);
                char* ack = convert_frame_to_char(inframe);
                ll_append_node(outgoing_frames_head_ptr, ack);

            } else if(is_in_window_recv(receiver, inframe)) {
                // concat msg and acknowledge
                // printf("msg buffer: %s\n", receiver->msg_buffer[inframe->src_id]);
                int NFE = receiver->sender_state[inframe->src_id].NFE;
                //printf("recv NFE: %d\n", NFE);

                // store into the window
                receiver->sender_state[inframe->src_id].recvQ[inframe->id % SWS].frame = inframe;
                receiver->sender_state[inframe->src_id].recvQ[inframe->id % SWS].received = 1;
                //receiver->LFR = inframe->id;
                receiver->prev_id[inframe->src_id] = inframe->id;

                // store into buffer and determine ack to send
                //strcat(receiver->msg_buffer[inframe->src_id], temp);
                Frame* inframe_cpy = malloc(sizeof(Frame));
                memcpy(inframe_cpy, inframe, sizeof(Frame));
                receiver->sender_state[inframe->src_id].buffer[inframe_cpy->id] = inframe_cpy;

                for(int i = NFE; i < NFE + RWS; i ++) {
                    if(receiver->sender_state[inframe->src_id].recvQ[i % SWS].received == 0 && i != 0) {
                        Frame* ack = malloc(sizeof(Frame));
                        ack->ack_id = i - 1;
                        ack->dst_id = receiver->recv_id;
                        //compute_crc(convert_frame_to_char(ack), inframe);
                        char* ack_char = convert_frame_to_char(ack);
                        receiver->sender_state[inframe->src_id].NFE = i;
                        //printf("recv send ack: %d\n", i - 1);
                        
                        ll_append_node(outgoing_frames_head_ptr, ack_char);
                        //free(ack);
                        break;
                    }
                    receiver->sender_state[inframe->src_id].recvQ[i % SWS].received = 0;
                    receiver->sender_state[inframe->src_id].NFE = i + 1;
                }

                // if end of message then print
                unsigned long length;
                char* res = reassemble_msg(receiver->sender_state[inframe->src_id].buffer, &length);
                if(res != NULL && strlen(res) == length) {
                    printf("<RECV_%d>:[%s]\n", receiver->recv_id, res);
                    
                    // clear buffer
                    for(int i = 0; i < MAX_BUFFER_LEN; i ++) {
                        //free(receiver->sender_state[inframe->src_id].buffer[i]);
                        receiver->sender_state[inframe->src_id].buffer[i] = NULL;
                    }

                    // // clear window
                    for(int i = 0; i < RWS; i ++) {
                        receiver->sender_state[inframe->src_id].recvQ[i] = (recvQ_slot){NULL, 0};
                    }
                }
            } 
        }
        
        // Free raw_char_buf
        // free(raw_char_buf);
        // free(inframe);
        // free(ll_inmsg_node);
    }
}

void* run_receiver(void* input_receiver) {
    struct timespec time_spec;
    struct timeval curr_timeval;
    const int WAIT_SEC_TIME = 0;
    const long WAIT_USEC_TIME = 100000;
    Receiver* receiver = (Receiver*) input_receiver;
    LLnode* outgoing_frames_head;

    // This incomplete receiver thread, at a high level, loops as follows:
    // 1. Determine the next time the thread should wake up if there is nothing
    // in the incoming queue(s)
    // 2. Grab the mutex protecting the input_msg queue
    // 3. Dequeues messages from the input_msg queue and prints them
    // 4. Releases the lock
    // 5. Sends out any outgoing messages

    pthread_cond_init(&receiver->buffer_cv, NULL);
    pthread_mutex_init(&receiver->buffer_mutex, NULL);

    while (1) {
        // NOTE: Add outgoing messages to the outgoing_frames_head pointer
        outgoing_frames_head = NULL;
        gettimeofday(&curr_timeval, NULL);

        // Either timeout or get woken up because you've received a datagram
        // NOTE: You don't really need to do anything here, but it might be
        // useful for debugging purposes to have the receivers periodically
        // wakeup and print info
        time_spec.tv_sec = curr_timeval.tv_sec;
        time_spec.tv_nsec = curr_timeval.tv_usec * 1000;
        time_spec.tv_sec += WAIT_SEC_TIME;
        time_spec.tv_nsec += WAIT_USEC_TIME * 1000;
        if (time_spec.tv_nsec >= 1000000000) {
            time_spec.tv_sec++;
            time_spec.tv_nsec -= 1000000000;
        }

        //*****************************************************************************************
        // NOTE: Anything that involves dequeing from the input frames should go
        //      between the mutex lock and unlock, because other threads
        //      CAN/WILL access these structures
        //*****************************************************************************************
        pthread_mutex_lock(&receiver->buffer_mutex);

        // Check whether anything arrived
        int incoming_msgs_length =
            ll_get_length(receiver->input_framelist_head);
        if (incoming_msgs_length == 0) {
            // Nothing has arrived, do a timed wait on the condition variable
            // (which releases the mutex). Again, you don't really need to do
            // the timed wait. A signal on the condition variable will wake up
            // the thread and reacquire the lock
            pthread_cond_timedwait(&receiver->buffer_cv,
                                   &receiver->buffer_mutex, &time_spec);
        }

        handle_incoming_msgs(receiver, &outgoing_frames_head);

        pthread_mutex_unlock(&receiver->buffer_mutex);

        // CHANGE THIS AT YOUR OWN RISK!
        // Send out all the frames user has appended to the outgoing_frames list
        int ll_outgoing_frame_length = ll_get_length(outgoing_frames_head);
        while (ll_outgoing_frame_length > 0) {
            LLnode* ll_outframe_node = ll_pop_node(&outgoing_frames_head);
            char* char_buf = (char*) ll_outframe_node->value;

            // The following function frees the memory for the char_buf object
            send_msg_to_senders(char_buf);

            // Free up the ll_outframe_node
            free(ll_outframe_node);

            ll_outgoing_frame_length = ll_get_length(outgoing_frames_head);
        }
    }
    pthread_exit(NULL);
}
