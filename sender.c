#include "sender.h"
#include <assert.h>
#include <stdint.h>

#define MICRO_SEC_LENGTH 1000000
#define TIME_OUT_VAL 90000
#define POLY 0x07
static unsigned char crc_table[256];

void create_crc_table() {
    uint8_t crc;

	for (int i = 0; i < 256; i++) {
	    crc = i;
	    for (int j = 0; j < 8; j++) {
	    	if(crc & 0x80) {
	    		crc = (crc << 1) ^ 0x07;
	    	} else {
	    		crc = (crc << 1);
	    	}
	    }
      	
      	crc_table[i] = crc & 0xFF;
	}
}

uint8_t compute_crc(char* char_arr, int length) {
    uint8_t crc = 0;
    uint8_t* data = (uint8_t*) char_arr;

    for(int i = 0; i < length; i ++) {
        crc = crc_table[(crc) ^ data[i]];
        crc &= 0xFF;
    }

    return crc;
}

void init_sender(Sender* sender, int id) {
    // TODO: You should fill in this function as necessary
    // pthread_cond_init(&sender->buffer_cv, NULL);
    // pthread_mutex_init(&sender->buffer_mutex, NULL);
    
    sender->send_id = id;
    sender->input_cmdlist_head = NULL;
    sender->input_framelist_head = NULL;
    sender->waiting_frames_head = NULL;
    sender->prev_cmd_recv = 0;

    // init state for receivers
    sender->receiver_state = calloc(sizeof(Recv_state), 
                                    glb_receivers_array_length);
    for(int i = 0; i < glb_receivers_array_length; i ++) {
        sender->receiver_state[i].LAR = 0;
        sender->receiver_state[i].LFS = 0;

        for(int j = 0; j < SWS; j ++) {
            sender->receiver_state[j].sendQ[j] = (sendQ_slot){NULL, NULL, 0};
        }
    }

    // init crc
    create_crc_table();
}

void set_timedout_time(struct sendQ_slot* slot) {
    if(slot->time_val == 0) {
        slot->time_val = malloc(sizeof(struct timeval));
    }

    gettimeofday(slot->time_val, NULL);
    int sec = slot->time_val->tv_sec;
    int m_sec = slot->time_val->tv_usec;
    
    // add time out value
    m_sec = m_sec + TIME_OUT_VAL;

    // check if overflow
    if(m_sec > MICRO_SEC_LENGTH) {
        m_sec %= MICRO_SEC_LENGTH;
        sec ++;
    }
    
    // update time_val
    slot->time_val->tv_sec = sec;
    slot->time_val->tv_usec = m_sec;
}

// store the frame in order for resend
void store_prev_frame(Sender* sender, Frame* outgoing_frame) {
    int frame_slot = outgoing_frame->id % SWS;
    sender->receiver_state[outgoing_frame->dst_id].sendQ[frame_slot].frame = outgoing_frame;
    sender->receiver_state[outgoing_frame->dst_id].sendQ[frame_slot].is_ack = 0;
    set_timedout_time(&(sender->receiver_state[outgoing_frame->dst_id].sendQ[frame_slot]));
    //printf("prev data: %s\n", outgoing_frame->data);
}

void add_header(Frame* outgoing_frame, Cmd* outgoing_cmd, Sender* sender) {
    outgoing_frame->dst_id = outgoing_cmd->dst_id;
    outgoing_frame->src_id = outgoing_cmd->src_id;
    outgoing_frame->ack_id = -1;
    sender->receiver_state[outgoing_frame->dst_id].LFS ++;
    outgoing_frame->id = sender->receiver_state[outgoing_frame->dst_id].LFS;
    outgoing_frame->length = strlen(outgoing_cmd->message);
    outgoing_frame->crc = compute_crc(convert_frame_to_char(outgoing_frame), 63);
    //printf("crc data: %d\n", outgoing_frame->crc);
    // fprintf(stderr, "LFS: %d\n", sender->LFS);
    // fprintf(stderr, "LAR: %d\n", sender->LAR);
}

struct timeval* sender_get_next_expiring_timeval(Sender* sender) {
    // TODO: You should fill in this function so that it returns the next
    // timeout that should occur
    // no time to find

    // loop over sendQ, find next time val
    struct timeval* cur_time = malloc(sizeof(struct timeval));
    gettimeofday(cur_time, NULL);
    struct timeval* next_time = malloc(sizeof(struct timeval));
    next_time->tv_sec = -1;
    next_time->tv_usec = -1;
    long diff = INT32_MAX;
    
    for(int j = 0; j < glb_receivers_array_length; j ++) {
        int LAR = sender->receiver_state[j].LAR;
        int LFS = sender->receiver_state[j].LFS;
        sendQ_slot* sendQ = sender->receiver_state->sendQ;
        
        if(LAR == LFS) continue;

        for(int i = LAR + 1; i <= LFS; i ++) {
            struct timeval* cur_slot_time = sendQ[i % SWS].time_val;
            if(cur_slot_time == NULL) continue;
            long cur_diff = timeval_usecdiff(cur_time, cur_slot_time);
            
            // if difference is smaller, update
            if(cur_diff < diff) {
                diff = cur_diff;
                next_time->tv_sec = cur_slot_time->tv_sec;
                next_time->tv_usec = cur_slot_time->tv_usec;
            }
        }
    }
    

    if(next_time->tv_sec == -1 || next_time->tv_usec == -1) return NULL;
    return next_time;

}

int is_in_window(Sender* sender, Frame* inframe) {
    int id = inframe->ack_id;
    if(id == -1) {
        return 0;
    }

    int LAR = sender->receiver_state[inframe->dst_id].LAR;
    int LFS = sender->receiver_state[inframe->dst_id].LFS;
    if((LAR < LFS) && (id > LAR && id <= LFS)) {
        return 1;
    }

    if((LAR > LFS) && (id > LAR || id <= LFS)) {
        return 1;
    }

    return 0;
}

void handle_incoming_acks(Sender* sender, LLnode** outgoing_frames_head_ptr) {
    // TODO: Suggested steps for handling incoming ACKs
    //    1) Dequeue the ACK from the sender->input_framelist_head
    //    2) Convert the char * buffer to a Frame data type
    //    3) Check whether the frame is for this sender
    //    4) Do stop-and-wait for sender/receiver pair
    int incoming_msgs_length = ll_get_length(sender->input_framelist_head);
    
    if(incoming_msgs_length == 0) {
        return;
    }

    while(incoming_msgs_length > 0) {
        // Ask yourself: Is this message really for me?
        LLnode* ll_inmsg_node = ll_pop_node(&sender->input_framelist_head);
        incoming_msgs_length = ll_get_length(sender->input_framelist_head);
        char* raw_char_buf = ll_inmsg_node->value;
        Frame* inframe = convert_char_to_frame(raw_char_buf);
        
        // if(is_corrupted(inframe)) {
        //     continue;
        // }

        // check if this frame is for me
        //fprintf(stdout, "is_in_window: %d\n", is_in_window(sender, inframe->ack_id));
        if(inframe->src_id == sender->send_id && 
           is_in_window(sender, inframe)) {
            // print out msg and acknowledge
            //fprintf(stdout, "acknowledgement received %d\n", inframe->ack_id);
            int dst_id = inframe->dst_id;
            Recv_state state = sender->receiver_state[dst_id];
            int LAR = state.LAR;
            int LFS = state.LFS;
            
            // slide window
            if(LAR < LFS || (LAR > LFS && inframe->ack_id > LAR)) {
                for(int i = LAR + 1; i <= inframe->ack_id; i ++) {
                    sender->receiver_state[dst_id].sendQ[i % SWS].is_ack = 1;

                    // free because recv get that
                    //free(sender->receiver_state[dst_id].sendQ[i % SWS].frame);
                    sender->receiver_state[dst_id].sendQ[i % SWS].frame = NULL;
                }
            } else {
                // ack_id overflow case
                if(LAR > LFS && 
                   inframe->ack_id < LAR &&
                   inframe->ack_id <= LFS) {
                    
                    for(int i = LAR + 1; i <= inframe->ack_id + SEQ_LENGTH; i ++) {
                        sender->receiver_state[dst_id].sendQ[i % SWS].is_ack = 1;

                        // free because recv get that
                        //free(sender->receiver_state[dst_id].sendQ[i % SWS].frame);
                        sender->receiver_state[dst_id].sendQ[i % SWS].frame = NULL;
                    }
                }
            }
            // update LAR
            sender->receiver_state[dst_id].LAR = inframe->ack_id;
                    
            LLnode* next_node = ll_pop_node(&sender->waiting_frames_head);
            
            // send next frames
            // no frames waiting
            if(next_node == 0) {
                return;
            }
            
            // send next frame
            char* next_frame = next_node->value;
            if(next_frame != 0) {
                ll_append_node(outgoing_frames_head_ptr, 
                               next_frame);

                // keep track of this frame for resend and record time
                Frame* outgoing_frame = convert_char_to_frame(next_frame);
                store_prev_frame(sender, outgoing_frame);
            }
            
            return;
        }
    }
}

void handle_input_cmds(Sender* sender, LLnode** outgoing_frames_head_ptr) {
    // TODO: Suggested steps for handling input cmd
    //    1) Dequeue the Cmd from sender->input_cmdlist_head
    //    2) Convert to Frame
    //    3) Set up the frame according to the protocol
    int input_cmd_length = ll_get_length(sender->input_cmdlist_head);
   
    // Recheck the command queue length to see if stdin_thread dumped a command
    // on us
    input_cmd_length = ll_get_length(sender->input_cmdlist_head);
    
    while (ll_get_length(sender->waiting_frames_head) == 0 &&
           (input_cmd_length > 0) &&
           (sender->receiver_state[sender->prev_cmd_recv].LAR == 
           sender->receiver_state[sender->prev_cmd_recv].LFS)) {
        
        // Pop a node off and update the input_cmd_length
        LLnode* ll_input_cmd_node = ll_pop_node(&sender->input_cmdlist_head);
        input_cmd_length = ll_get_length(sender->input_cmdlist_head);

        // Cast to Cmd type and free up the memory for the node
        Cmd* outgoing_cmd = (Cmd*) ll_input_cmd_node->value;
        free(ll_input_cmd_node);

        // DUMMY CODE: Add the raw char buf to the outgoing_frames list
        // NOTE: You should not blindly send this message out!
        //      Ask yourself: Is this message actually going to the right
        //      receiver (recall that default behavior of send is to broadcast
        //      to all receivers)?
        //                    Were the previous messages sent to this receiver
        //                    ACTUALLY delivered to the receiver?
        int msg_length = strlen(outgoing_cmd->message);
        if (msg_length > FRAME_PAYLOAD_SIZE) {
            // Do something about messages that exceed the frame size
            // use loop to attatch fixed sized frame to linked list
            int current_length = msg_length;
            char* current_pos = outgoing_cmd->message;
            int cnt = 0;
            sender->prev_cmd_recv = outgoing_cmd->dst_id;
            while(current_length > 0) {
                // create frame 
                Frame* outgoing_frame = calloc(sizeof(Frame), 1);
                assert(outgoing_frame);
                
                // divide cmd into many frames
                if(current_length > FRAME_PAYLOAD_SIZE) {
                    strncpy(outgoing_frame->data, current_pos, FRAME_PAYLOAD_SIZE);
                    current_pos += FRAME_PAYLOAD_SIZE;
                    current_length -= FRAME_PAYLOAD_SIZE;
                    outgoing_frame->is_end = 0;
                } else {
                    strncpy(outgoing_frame->data, current_pos, current_length + 1);
                    current_pos += current_length;
                    current_length -= current_length;
                    outgoing_frame->is_end = 1;
                }
                
                // add frame header
                add_header(outgoing_frame, outgoing_cmd, sender);

                //Convert the message to the outgoing_charbuf, only send the first 8 frame
                if(cnt < SWS) {
                    char* outgoing_charbuf = convert_frame_to_char(outgoing_frame);
                    ll_append_node(outgoing_frames_head_ptr, outgoing_charbuf);
                    
                    // keep track of this frame for resend
                    store_prev_frame(sender, outgoing_frame);
                } else {
                    ll_append_node(&sender->waiting_frames_head, 
                                   convert_frame_to_char(outgoing_frame));
                }
                
                cnt ++;
            }

            // At this point, we don't need the outgoing_cmd
            free(outgoing_cmd->message);
            free(outgoing_cmd);
            
        } else {
            // This is probably ONLY one step you want
            Frame* outgoing_frame = malloc(sizeof(Frame));
            assert(outgoing_frame);
            strcpy(outgoing_frame->data, outgoing_cmd->message);

            // add frame header
            outgoing_frame->is_end = 1;
            add_header(outgoing_frame, outgoing_cmd, sender);
            
            // At this point, we don't need the outgoing_cmd
            free(outgoing_cmd->message);
            free(outgoing_cmd);

            // Convert the message to the outgoing_charbuf
            char* outgoing_charbuf = convert_frame_to_char(outgoing_frame);
            ll_append_node(outgoing_frames_head_ptr, outgoing_charbuf);
            
            // keep track of this frame for resend
            store_prev_frame(sender, outgoing_frame);
        }
    }
}

void handle_timedout_frames(Sender* sender, LLnode** outgoing_frames_head_ptr) {
    //TODO: Handle timeout by resending the appropriate message
    //check if time val is intialized

    //no id overflow
    for(int k = 0; k < glb_receivers_array_length; k ++) {
        int LAR = sender->receiver_state[k].LAR;
        int LFS = sender->receiver_state[k].LFS;

        if(LAR == LFS) continue;

        if(LAR < LFS) {
            for(int i = LAR + 1; i <= LFS; i ++) {
                sendQ_slot cur_slot = sender->receiver_state[k].sendQ[i % SWS];
                if(cur_slot.time_val == NULL) continue;
                int sender_sec = cur_slot.time_val->tv_sec;
                int sender_m_sec = cur_slot.time_val->tv_usec;

                // calculate curr time
                struct timeval* current_time = malloc(sizeof(struct timeval));
                gettimeofday(current_time, NULL);
                int cur_sec = current_time->tv_sec;
                int cur_m_sec = current_time->tv_usec;

                //printf("is_ack: %d\n", cur_slot.is_ack);
                // check if overtime, if overtime then resend and set timer
                if(cur_slot.is_ack == 0 && ((sender_sec == cur_sec && cur_m_sec > sender_m_sec) ||
                (sender_sec < cur_sec))) {
                    // go back N
                    // printf("frame lost resend\n");
                    for(int j = i; j <= LFS; j ++) {
                        sendQ_slot resend_slot = sender->receiver_state[k].sendQ[j % SWS];
                        if(resend_slot.frame != NULL && resend_slot.is_ack == 0) {
                            // resend msg
                            Frame* frame_to_resend = resend_slot.frame; 
                            char* outgoing_charbuf = convert_frame_to_char(frame_to_resend);
                            ll_append_node(outgoing_frames_head_ptr, outgoing_charbuf);
                            //printf("resend message\n");   
                            resend_slot.is_ack = 0;
                            set_timedout_time(&resend_slot);
                        }
                    }
                }

                free(current_time);
            }
        } else {
            for(int i = LAR + 1; i <= LFS + SEQ_LENGTH; i ++) {
                sendQ_slot cur_slot = sender->receiver_state[k].sendQ[i % SWS];
                if(cur_slot.time_val == NULL) continue;
                int sender_sec = cur_slot.time_val->tv_sec;
                int sender_m_sec = cur_slot.time_val->tv_usec;

                // calculate curr time
                struct timeval* current_time = malloc(sizeof(struct timeval));
                gettimeofday(current_time, NULL);
                int cur_sec = current_time->tv_sec;
                int cur_m_sec = current_time->tv_usec;

                // check if overtime, if overtime then resend and set timer
                if(cur_slot.is_ack == 0 && ((sender_sec == cur_sec && cur_m_sec > sender_m_sec) ||
                (sender_sec < cur_sec))) {
                    // go back N
                    for(int j = i; j <= LFS + SEQ_LENGTH; j ++) {
                        sendQ_slot resend_slot = sender->receiver_state[k].sendQ[j % SWS];
                        if(resend_slot.frame != NULL && resend_slot.is_ack == 0) {
                            // resend msg
                            Frame* frame_to_resend = resend_slot.frame; 
                            char* outgoing_charbuf = convert_frame_to_char(frame_to_resend);
                            ll_append_node(outgoing_frames_head_ptr, outgoing_charbuf);
                            //printf("resend message \n");   
                            resend_slot.is_ack = 0;
                            set_timedout_time(&resend_slot);
                        }
                    }
                }

                free(current_time);
            }
        }
    }
    
    
    return;
}

void* run_sender(void* input_sender) {
    struct timespec time_spec;
    struct timeval curr_timeval;
    const int WAIT_SEC_TIME = 0;
    const long WAIT_USEC_TIME = 100000;
    Sender* sender = (Sender*) input_sender;
    LLnode* outgoing_frames_head;
    struct timeval* expiring_timeval;
    long sleep_usec_time, sleep_sec_time;

    // This incomplete sender thread, at a high level, loops as follows:
    // 1. Determine the next time the thread should wake up
    // 2. Grab the mutex protecting the input_cmd/inframe queues
    // 3. Dequeues messages from the input queue and adds them to the
    // outgoing_frames list
    // 4. Releases the lock
    // 5. Sends out the messages

    pthread_cond_init(&sender->buffer_cv, NULL);
    pthread_mutex_init(&sender->buffer_mutex, NULL);

    while (1) {
        outgoing_frames_head = NULL;

        // Get the current time
        gettimeofday(&curr_timeval, NULL);

        // time_spec is a data structure used to specify when the thread should
        // wake up The time is specified as an ABSOLUTE (meaning, conceptually,
        // you specify 9/23/2010 @ 1pm, wakeup)
        time_spec.tv_sec = curr_timeval.tv_sec;
        time_spec.tv_nsec = curr_timeval.tv_usec * 1000;

        // Check for the next event we should handle
        expiring_timeval = sender_get_next_expiring_timeval(sender);

        // Perform full on timeout
        if (expiring_timeval == NULL) {
            time_spec.tv_sec += WAIT_SEC_TIME;
            time_spec.tv_nsec += WAIT_USEC_TIME * 1000;
        } else {
            // Take the difference between the next event and the current time
            sleep_usec_time = timeval_usecdiff(&curr_timeval, expiring_timeval);

            // Sleep if the difference is positive
            if (sleep_usec_time > 0) {
                sleep_sec_time = sleep_usec_time / 1000000;
                sleep_usec_time = sleep_usec_time % 1000000;
                time_spec.tv_sec += sleep_sec_time;
                time_spec.tv_nsec += sleep_usec_time * 1000;
            }
        }

        // Check to make sure we didn't "overflow" the nanosecond field
        if (time_spec.tv_nsec >= 1000000000) {
            time_spec.tv_sec++;
            time_spec.tv_nsec -= 1000000000;
        }

        //*****************************************************************************************
        // NOTE: Anything that involves dequeing from the input frames or input
        // commands should go
        //      between the mutex lock and unlock, because other threads
        //      CAN/WILL access these structures
        //*****************************************************************************************
        pthread_mutex_lock(&sender->buffer_mutex);

        // Check whether anything has arrived
        int input_cmd_length = ll_get_length(sender->input_cmdlist_head);
        int inframe_queue_length = ll_get_length(sender->input_framelist_head);

        // Nothing (cmd nor incoming frame) has arrived, so do a timed wait on
        // the sender's condition variable (releases lock) A signal on the
        // condition variable will wakeup the thread and reaquire the lock
        if (input_cmd_length == 0 && inframe_queue_length == 0) {
            pthread_cond_timedwait(&sender->buffer_cv, &sender->buffer_mutex,
                                   &time_spec);
        }
        // Implement this
        handle_incoming_acks(sender, &outgoing_frames_head);

        // Implement this
        handle_input_cmds(sender, &outgoing_frames_head);
        
        pthread_mutex_unlock(&sender->buffer_mutex);

        // Implement this
        handle_timedout_frames(sender, &outgoing_frames_head);
        
        // CHANGE THIS AT YOUR OWN RISK!
        // Send out all the frames
        int ll_outgoing_frame_length = ll_get_length(outgoing_frames_head);

        while (ll_outgoing_frame_length > 0) {
            LLnode* ll_outframe_node = ll_pop_node(&outgoing_frames_head);
            char* char_buf = (char*) ll_outframe_node->value;

            // Don't worry about freeing the char_buf, the following function
            // does that
            send_msg_to_receivers(char_buf);

            // Free up the ll_outframe_node
            free(ll_outframe_node);

            ll_outgoing_frame_length = ll_get_length(outgoing_frames_head);
        }
    }
    pthread_exit(NULL);
    return 0;
}
