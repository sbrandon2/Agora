/**
 * @file agora.cc
 * @brief Implementation file for the main agora class
 */

#include "agora.h"

#include <cmath>
#include <memory>

static const bool kDebugDeferral = true;
static const size_t kDefaultMessageQueueSize = 512;
static const size_t kDefaultWorkerQueueSize = 256;

Agora::Agora(Config* const cfg)
    : base_worker_core_offset_(cfg->CoreOffset() + 1 + cfg->SocketThreadNum()),
      config_(cfg),
      stats_(std::make_unique<Stats>(cfg)),
      phy_stats_(std::make_unique<PhyStats>(cfg, Direction::kUplink)),
      csi_buffers_(kFrameWnd, cfg->UeAntNum(),
                   cfg->BsAntNum() * cfg->OfdmDataNum()),
      ul_zf_matrices_(kFrameWnd, cfg->OfdmDataNum(),
                      cfg->BsAntNum() * cfg->UeAntNum()),
      demod_buffers_(kFrameWnd, cfg->Frame().NumULSyms(), cfg->UeAntNum(),
                     kMaxModType * cfg->OfdmDataNum()),
      decoded_buffer_(kFrameWnd, cfg->Frame().NumULSyms(), cfg->UeAntNum(),
                      cfg->LdpcConfig().NumBlocksInSymbol() *
                          Roundup<64>(cfg->NumBytesPerCb())),
      dl_zf_matrices_(kFrameWnd, cfg->OfdmDataNum(),
                      cfg->UeAntNum() * cfg->BsAntNum()) {
  std::string directory = TOSTRING(PROJECT_DIRECTORY);
  std::printf("Agora: project directory [%s], RDTSC frequency = %.2f GHz\n",
              directory.c_str(), cfg->FreqGhz());

  PinToCoreWithOffset(ThreadType::kMaster, cfg->CoreOffset(), 0,
                      false /* quiet */);
  CheckIncrementScheduleFrame(0, ScheduleProcessingFlags::kProcessingComplete);
  // Important to set cur_sche_frame_id_ after the call to
  // CheckIncrementScheduleFrame because it will be incremented however,
  // CheckIncrementScheduleFrame will initialize the schedule tracking variable
  // correctly.
  cur_sche_frame_id_ = 0;
  cur_proc_frame_id_ = 0;

  InitializeQueues();
  InitializeUplinkBuffers();
  InitializeDownlinkBuffers();

  /* Initialize TXRX threads */
  packet_tx_rx_ = std::make_unique<PacketTXRX>(
      cfg, cfg->CoreOffset() + 1, &message_queue_,
      GetConq(EventType::kPacketTX, 0), rx_ptoks_ptr_, tx_ptoks_ptr_);

  if (kEnableMac == true) {
    const size_t mac_cpu_core =
        cfg->CoreOffset() + cfg->SocketThreadNum() + cfg->WorkerThreadNum() + 1;
    mac_thread_ = std::make_unique<MacThreadBaseStation>(
        cfg, mac_cpu_core, decoded_buffer_, &dl_bits_buffer_,
        &dl_bits_buffer_status_, &mac_request_queue_, &mac_response_queue_);

    mac_std_thread_ =
        std::thread(&MacThreadBaseStation::RunEventLoop, mac_thread_.get());
  }

  // Create worker threads
  CreateThreads();

  MLPD_INFO(
      "Master thread core %zu, TX/RX thread cores %zu--%zu, worker thread "
      "cores %zu--%zu\n",
      cfg->CoreOffset(), cfg->CoreOffset() + 1,
      cfg->CoreOffset() + 1 + cfg->SocketThreadNum() - 1,
      base_worker_core_offset_,
      base_worker_core_offset_ + cfg->WorkerThreadNum() - 1);
}

Agora::~Agora() {
  if (kEnableMac == true) {
    mac_std_thread_.join();
  }

  for (auto& worker_thread : workers_) {
    MLPD_SYMBOL("Agora: Joining worker thread\n");
    worker_thread.join();
  }
  FreeUplinkBuffers();
  FreeDownlinkBuffers();

  stats_.reset();
  phy_stats_.reset();
  FreeQueues();
}

void Agora::Stop() {
  MLPD_INFO("Agora: terminating\n");
  config_->Running(false);
  usleep(1000);
  packet_tx_rx_.reset();
}

void Agora::SendSnrReport(EventType event_type, size_t frame_id,
                          size_t symbol_id) {
  assert(event_type == EventType::kSNRReport);
  unused(event_type);
  auto base_tag = gen_tag_t::FrmSymUe(frame_id, symbol_id, 0);
  for (size_t i = 0; i < config_->UeAntNum(); i++) {
    EventData snr_report(EventType::kSNRReport, base_tag.tag_);
    snr_report.num_tags_ = 2;
    float snr = this->phy_stats_->GetEvmSnr(frame_id, i);
    std::memcpy(&snr_report.tags_[1], &snr, sizeof(float));
    TryEnqueueFallback(&mac_request_queue_, snr_report);
    base_tag.ue_id_++;
  }
}

void Agora::ScheduleDownlinkProcessing(size_t frame_id) {
  size_t num_pilot_symbols = config_->Frame().ClientDlPilotSymbols();

  for (size_t i = 0; i < num_pilot_symbols; i++) {
    if (zf_last_frame_ == frame_id) {
      ScheduleSubcarriers(EventType::kPrecode, frame_id,
                          config_->Frame().GetDLSymbol(i));
    } else {
      encode_cur_frame_for_symbol_.at(i) = frame_id;
    }
  }

  for (size_t i = num_pilot_symbols; i < config_->Frame().NumDLSyms(); i++) {
    ScheduleCodeblocks(EventType::kEncode, frame_id,
                       config_->Frame().GetDLSymbol(i));
  }
}

void Agora::ScheduleAntennas(EventType event_type, size_t frame_id,
                             size_t symbol_id) {
  assert(event_type == EventType::kFFT or event_type == EventType::kIFFT);
  auto base_tag = gen_tag_t::FrmSymAnt(frame_id, symbol_id, 0);

  size_t num_blocks = config_->BsAntNum() / config_->FftBlockSize();
  size_t num_remainder = config_->BsAntNum() % config_->FftBlockSize();
  if (num_remainder > 0) {
    num_blocks++;
  }
  EventData event;
  event.num_tags_ = config_->FftBlockSize();
  event.event_type_ = event_type;
  size_t qid = frame_id & 0x1;
  for (size_t i = 0; i < num_blocks; i++) {
    if ((i == num_blocks - 1) && num_remainder > 0) {
      event.num_tags_ = num_remainder;
    }
    for (size_t j = 0; j < event.num_tags_; j++) {
      event.tags_[j] = base_tag.tag_;
      base_tag.ant_id_++;
    }
    TryEnqueueFallback(GetConq(event_type, qid), GetPtok(event_type, qid),
                       event);
  }
}

void Agora::ScheduleAntennasTX(size_t frame_id, size_t symbol_id) {
  auto base_tag = gen_tag_t::FrmSymAnt(frame_id, symbol_id, 0);
  const size_t total_antennas = config_->BsAntNum();
  const size_t handler_threads = config_->SocketThreadNum();
  size_t schedule_antenna = 0;

  const size_t rem_antennas = total_antennas % handler_threads;
  const size_t floor_events_per_handler = total_antennas / handler_threads;
  size_t ceil_events_per_handler = floor_events_per_handler;
  if (rem_antennas > 0) {
    ceil_events_per_handler += 1;
  }
  // Must put contiguous channels in same queue
  assert(ceil_events_per_handler % config_->NumChannels() == 0);

  std::vector<EventData> events_list(ceil_events_per_handler);
  for (size_t radio_handler = 0; radio_handler < handler_threads;
       radio_handler++) {
    size_t tx_event;
    for (tx_event = 0; tx_event < ceil_events_per_handler; tx_event++) {
      if (schedule_antenna == total_antennas) {
        // All antennas scheduled
        break;
      }

      events_list.at(tx_event).num_tags_ = 1;
      events_list.at(tx_event).event_type_ = EventType::kPacketTX;
      events_list.at(tx_event).tags_[0u] = base_tag.tag_;
      base_tag.ant_id_ = ++schedule_antenna;
    }
    TryEnqueueBulkFallback(GetConq(EventType::kPacketTX, 0),
                           tx_ptoks_ptr_[radio_handler], events_list.data(),
                           tx_event);
  }
}

void Agora::ScheduleSubcarriers(EventType event_type, size_t frame_id,
                                size_t symbol_id) {
  auto base_tag = gen_tag_t::FrmSymSc(frame_id, symbol_id, 0);
  size_t num_events = SIZE_MAX;
  size_t block_size = SIZE_MAX;

  switch (event_type) {
    case EventType::kDemul:
    case EventType::kPrecode:
      num_events = config_->DemulEventsPerSymbol();
      block_size = config_->DemulBlockSize();
      break;
    case EventType::kZF:
      num_events = config_->ZfEventsPerSymbol();
      block_size = config_->ZfBlockSize();
      break;
    default:
      assert(false);
  }

  size_t qid = (frame_id & 0x1);
  if (event_type == EventType::kZF) {
    EventData event;
    event.event_type_ = event_type;
    event.num_tags_ = config_->ZfBatchSize();
    size_t num_blocks = num_events / event.num_tags_;
    size_t num_remainder = num_events % event.num_tags_;
    if (num_remainder > 0) {
      num_blocks++;
    }
    for (size_t i = 0; i < num_blocks; i++) {
      if ((i == num_blocks - 1) && num_remainder > 0) {
        event.num_tags_ = num_remainder;
      }
      for (size_t j = 0; j < event.num_tags_; j++) {
        event.tags_[j] =
            gen_tag_t::FrmSymSc(frame_id, symbol_id,
                                block_size * (i * event.num_tags_ + j))
                .tag_;
      }
      TryEnqueueFallback(GetConq(event_type, qid), GetPtok(event_type, qid),
                         event);
    }
  } else {
    for (size_t i = 0; i < num_events; i++) {
      TryEnqueueFallback(GetConq(event_type, qid), GetPtok(event_type, qid),
                         EventData(event_type, base_tag.tag_));
      base_tag.sc_id_ += block_size;
    }
  }
}

void Agora::ScheduleCodeblocks(EventType event_type, size_t frame_id,
                               size_t symbol_idx) {
  auto base_tag = gen_tag_t::FrmSymCb(frame_id, symbol_idx, 0);
  const size_t num_tasks =
      config_->UeAntNum() * config_->LdpcConfig().NumBlocksInSymbol();
  size_t num_blocks = num_tasks / config_->EncodeBlockSize();
  const size_t num_remainder = num_tasks % config_->EncodeBlockSize();
  if (num_remainder > 0) {
    num_blocks++;
  }
  EventData event;
  event.num_tags_ = config_->EncodeBlockSize();
  event.event_type_ = event_type;
  size_t qid = frame_id & 0x1;
  for (size_t i = 0; i < num_blocks; i++) {
    if ((i == num_blocks - 1) && num_remainder > 0) {
      event.num_tags_ = num_remainder;
    }
    for (size_t j = 0; j < event.num_tags_; j++) {
      event.tags_[j] = base_tag.tag_;
      base_tag.cb_id_++;
    }
    TryEnqueueFallback(GetConq(event_type, qid), GetPtok(event_type, qid),
                       event);
  }
}

void Agora::ScheduleUsers(EventType event_type, size_t frame_id,
                          size_t symbol_id) {
  assert(event_type == EventType::kPacketToMac);
  unused(event_type);
  auto base_tag = gen_tag_t::FrmSymUe(frame_id, symbol_id, 0);

  for (size_t i = 0; i < config_->UeAntNum(); i++) {
    TryEnqueueFallback(&mac_request_queue_,
                       EventData(EventType::kPacketToMac, base_tag.tag_));
    base_tag.ue_id_++;
  }
}

void Agora::Start() {
  const auto& cfg = this->config_;

  // Start packet I/O
  if (packet_tx_rx_->StartTxRx(socket_buffer_,
                               socket_buffer_size_ / cfg->PacketLength(),
                               this->stats_->FrameStart(), dl_socket_buffer_,
                               calib_dl_buffer_, calib_ul_buffer_) == false) {
    this->Stop();
    return;
  }

  PinToCoreWithOffset(ThreadType::kMaster, cfg->CoreOffset(), 0);

  // Counters for printing summary
  size_t tx_count = 0;
  double tx_begin = GetTime::GetTimeUs();

  bool is_turn_to_dequeue_from_io = true;
  const size_t max_events_needed =
      std::max(kDequeueBulkSizeTXRX * (cfg->SocketThreadNum() + 1 /* MAC */),
               kDequeueBulkSizeWorker * cfg->WorkerThreadNum());
  EventData events_list[max_events_needed];

  while ((config_->Running() == true) &&
         (SignalHandler::GotExitSignal() == false)) {
    // Get a batch of events
    size_t num_events = 0;
    if (is_turn_to_dequeue_from_io) {
      for (size_t i = 0; i < cfg->SocketThreadNum(); i++) {
        num_events += message_queue_.try_dequeue_bulk_from_producer(
            *(rx_ptoks_ptr_[i]), events_list + num_events,
            kDequeueBulkSizeTXRX);
      }

      if (kEnableMac == true) {
        num_events += mac_response_queue_.try_dequeue_bulk(
            events_list + num_events, kDequeueBulkSizeTXRX);
      }
    } else {
      num_events +=
          complete_task_queue_[(this->cur_proc_frame_id_ & 0x1)]
              .try_dequeue_bulk(events_list + num_events, max_events_needed);
    }
    is_turn_to_dequeue_from_io = !is_turn_to_dequeue_from_io;

    // Handle each event
    for (size_t ev_i = 0; ev_i < num_events; ev_i++) {
      EventData& event = events_list[ev_i];

      // FFT processing is scheduled after falling through the switch
      switch (event.event_type_) {
        case EventType::kPacketRX: {
          Packet* pkt = rx_tag_t(event.tags_[0]).rx_packet_->RawPacket();

          if (pkt->frame_id_ >= ((this->cur_sche_frame_id_ + kFrameWnd))) {
            MLPD_ERROR(
                "Error: Received packet for future frame %u beyond "
                "frame window (= %zu + %zu). This can happen if "
                "Agora is running slowly, e.g., in debug mode\n",
                pkt->frame_id_, this->cur_sche_frame_id_, kFrameWnd);
            cfg->Running(false);
            break;
          }

          UpdateRxCounters(pkt->frame_id_, pkt->symbol_id_);
          fft_queue_arr_[pkt->frame_id_ % kFrameWnd].push(
              fft_req_tag_t(event.tags_[0]));
        } break;

        case EventType::kFFT: {
          for (size_t i = 0; i < event.num_tags_; i++) {
            HandleEventFft(event.tags_[i]);
          }
        } break;

        case EventType::kZF: {
          for (size_t tag_id = 0; (tag_id < event.num_tags_); tag_id++) {
            size_t frame_id = gen_tag_t(event.tags_[tag_id]).frame_id_;
            PrintPerTaskDone(PrintType::kZF, frame_id, 0,
                             zf_counters_.GetTaskCount(frame_id));
            bool last_zf_task = this->zf_counters_.CompleteTask(frame_id);
            if (last_zf_task == true) {
              this->stats_->MasterSetTsc(TsType::kZFDone, frame_id);
              zf_last_frame_ = frame_id;
              PrintPerFrameDone(PrintType::kZF, frame_id);
              this->zf_counters_.Reset(frame_id);
              if (kPrintZfStats) {
                this->phy_stats_->PrintZfStats(frame_id);
              }

              for (size_t i = 0; i < cfg->Frame().NumULSyms(); i++) {
                if (this->fft_cur_frame_for_symbol_.at(i) == frame_id) {
                  ScheduleSubcarriers(EventType::kDemul, frame_id,
                                      cfg->Frame().GetULSymbol(i));
                }
              }
              // Schedule precoding for downlink symbols
              for (size_t i = 0; i < cfg->Frame().NumDLSyms(); i++) {
                size_t last_encoded_frame =
                    this->encode_cur_frame_for_symbol_.at(i);
                if ((last_encoded_frame != SIZE_MAX) &&
                    (last_encoded_frame >= frame_id)) {
                  ScheduleSubcarriers(EventType::kPrecode, frame_id,
                                      cfg->Frame().GetDLSymbol(i));
                }
              }
            }  // end if (zf_counters_.last_task(frame_id) == true)
          }
        } break;

        case EventType::kDemul: {
          size_t frame_id = gen_tag_t(event.tags_[0]).frame_id_;
          size_t symbol_id = gen_tag_t(event.tags_[0]).symbol_id_;
          size_t base_sc_id = gen_tag_t(event.tags_[0]).sc_id_;

          PrintPerTaskDone(PrintType::kDemul, frame_id, symbol_id, base_sc_id);
          bool last_demul_task =
              this->demul_counters_.CompleteTask(frame_id, symbol_id);

          if (last_demul_task == true) {
            ScheduleCodeblocks(EventType::kDecode, frame_id, symbol_id);
            PrintPerSymbolDone(PrintType::kDemul, frame_id, symbol_id);
            bool last_demul_symbol =
                this->demul_counters_.CompleteSymbol(frame_id);
            if (last_demul_symbol == true) {
              this->demul_counters_.Reset(frame_id);
              max_equaled_frame_ = frame_id;
              if (cfg->BigstationMode() == false) {
                assert(cur_sche_frame_id_ == frame_id);
                CheckIncrementScheduleFrame(frame_id, kUplinkComplete);
              } else {
                ScheduleCodeblocks(EventType::kDecode, frame_id, symbol_id);
              }
              this->stats_->MasterSetTsc(TsType::kDemulDone, frame_id);
              PrintPerFrameDone(PrintType::kDemul, frame_id);
            }
          }
        } break;

        case EventType::kDecode: {
          size_t frame_id = gen_tag_t(event.tags_[0]).frame_id_;
          size_t symbol_id = gen_tag_t(event.tags_[0]).symbol_id_;

          bool last_decode_task =
              this->decode_counters_.CompleteTask(frame_id, symbol_id);
          if (last_decode_task == true) {
            if (kEnableMac == true) {
              ScheduleUsers(EventType::kPacketToMac, frame_id, symbol_id);
            }
            PrintPerSymbolDone(PrintType::kDecode, frame_id, symbol_id);
            bool last_decode_symbol =
                this->decode_counters_.CompleteSymbol(frame_id);
            if (last_decode_symbol == true) {
              this->stats_->MasterSetTsc(TsType::kDecodeDone, frame_id);
              PrintPerFrameDone(PrintType::kDecode, frame_id);
              if (kEnableMac == false) {
                assert(this->cur_proc_frame_id_ == frame_id);
                bool work_finished = this->CheckFrameComplete(frame_id);
                if (work_finished == true) {
                  goto finish;
                }
              }
            }
          }
        } break;

        case EventType::kRANUpdate: {
          RanConfig rc;
          rc.n_antennas_ = event.tags_[0];
          rc.mod_order_bits_ = event.tags_[1];
          rc.frame_id_ = event.tags_[2];
          UpdateRanConfig(rc);
        } break;

        case EventType::kPacketToMac: {
          size_t frame_id = gen_tag_t(event.tags_[0]).frame_id_;
          size_t symbol_id = gen_tag_t(event.tags_[0]).symbol_id_;

          bool last_tomac_task =
              this->tomac_counters_.CompleteTask(frame_id, symbol_id);
          if (last_tomac_task == true) {
            PrintPerSymbolDone(PrintType::kPacketToMac, frame_id, symbol_id);

            bool last_tomac_symbol =
                this->tomac_counters_.CompleteSymbol(frame_id);
            if (last_tomac_symbol == true) {
              assert(this->cur_proc_frame_id_ == frame_id);
              // this->stats_->MasterSetTsc(TsType::kMacTXDone, frame_id);
              PrintPerFrameDone(PrintType::kPacketToMac, frame_id);
              bool work_finished = this->CheckFrameComplete(frame_id);
              if (work_finished == true) {
                goto finish;
              }
            }
          }

        } break;

        case EventType::kPacketFromMac: {
          size_t frame_id = rx_mac_tag_t(event.tags_[0]).offset_;

          bool last_ue = this->mac_to_phy_counters_.CompleteTask(frame_id, 0);
          if (last_ue == true) {
            // schedule this frame's encoding
            // Defer the schedule.  If frames are already deferred or the
            // current received frame is too far off
            if ((this->encode_deferral_.empty() == false) ||
                (frame_id >= (this->cur_proc_frame_id_ + kScheduleQueues))) {
              if (kDebugDeferral) {
                std::printf("   +++ Deferring encoding of frame %zu\n",
                            frame_id);
              }
              this->encode_deferral_.push(frame_id);
            } else {
              ScheduleDownlinkProcessing(frame_id);
            }
            this->mac_to_phy_counters_.Reset(frame_id);
            PrintPerFrameDone(PrintType::kPacketFromMac, frame_id);
          }
        } break;

        case EventType::kEncode: {
          for (size_t i = 0; i < event.num_tags_; i++) {
            size_t frame_id = gen_tag_t(event.tags_[i]).frame_id_;
            size_t symbol_id = gen_tag_t(event.tags_[i]).symbol_id_;

            bool last_encode_task =
                encode_counters_.CompleteTask(frame_id, symbol_id);
            if (last_encode_task == true) {
              this->encode_cur_frame_for_symbol_.at(
                  cfg->Frame().GetDLSymbolIdx(symbol_id)) = frame_id;
              // If precoder of the current frame exists
              if (zf_last_frame_ == frame_id) {
                ScheduleSubcarriers(EventType::kPrecode, frame_id, symbol_id);
              }
              PrintPerSymbolDone(PrintType::kEncode, frame_id, symbol_id);

              bool last_encode_symbol =
                  this->encode_counters_.CompleteSymbol(frame_id);
              if (last_encode_symbol == true) {
                this->encode_counters_.Reset(frame_id);
                this->stats_->MasterSetTsc(TsType::kEncodeDone, frame_id);
                PrintPerFrameDone(PrintType::kEncode, frame_id);
              }
            }
          }
        } break;

        case EventType::kPrecode: {
          // Precoding is done, schedule ifft
          size_t sc_id = gen_tag_t(event.tags_[0]).sc_id_;
          size_t frame_id = gen_tag_t(event.tags_[0]).frame_id_;
          size_t symbol_id = gen_tag_t(event.tags_[0]).symbol_id_;
          PrintPerTaskDone(PrintType::kPrecode, frame_id, symbol_id, sc_id);
          bool last_precode_task =
              this->precode_counters_.CompleteTask(frame_id, symbol_id);

          if (last_precode_task == true) {
            // precode_cur_frame_for_symbol_.at(
            //    this->config_->Frame().GetDLSymbolIdx(symbol_id)) = frame_id;
            ScheduleAntennas(EventType::kIFFT, frame_id, symbol_id);
            PrintPerSymbolDone(PrintType::kPrecode, frame_id, symbol_id);

            bool last_precode_symbol =
                this->precode_counters_.CompleteSymbol(frame_id);
            if (last_precode_symbol == true) {
              this->precode_counters_.Reset(frame_id);
              this->stats_->MasterSetTsc(TsType::kPrecodeDone, frame_id);
              PrintPerFrameDone(PrintType::kPrecode, frame_id);
            }
          }
        } break;

        case EventType::kIFFT: {
          for (size_t i = 0; i < event.num_tags_; i++) {
            /* IFFT is done, schedule data transmission */
            size_t ant_id = gen_tag_t(event.tags_[i]).ant_id_;
            size_t frame_id = gen_tag_t(event.tags_[i]).frame_id_;
            size_t symbol_id = gen_tag_t(event.tags_[i]).symbol_id_;
            size_t symbol_idx_dl = cfg->Frame().GetDLSymbolIdx(symbol_id);
            PrintPerTaskDone(PrintType::kIFFT, frame_id, symbol_id, ant_id);

            bool last_ifft_task =
                this->ifft_counters_.CompleteTask(frame_id, symbol_id);
            if (last_ifft_task == true) {
              ifft_cur_frame_for_symbol_.at(symbol_idx_dl) = frame_id;
              if (symbol_idx_dl == ifft_next_symbol_) {
                // Check the available symbols starting from the current symbol
                // Only schedule symbols that are continuously available
                for (size_t sym_id = symbol_idx_dl;
                     sym_id <= ifft_counters_.GetSymbolCount(frame_id);
                     sym_id++) {
                  size_t symbol_ifft_frame =
                      ifft_cur_frame_for_symbol_.at(sym_id);
                  if (symbol_ifft_frame == frame_id) {
                    ScheduleAntennasTX(frame_id,
                                       cfg->Frame().GetDLSymbol(sym_id));
                    ifft_next_symbol_++;
                  } else {
                    break;
                  }
                }
              }
              PrintPerSymbolDone(PrintType::kIFFT, frame_id, symbol_id);

              bool last_ifft_symbol =
                  this->ifft_counters_.CompleteSymbol(frame_id);
              if (last_ifft_symbol == true) {
                ifft_next_symbol_ = 0;
                this->stats_->MasterSetTsc(TsType::kIFFTDone, frame_id);
                PrintPerFrameDone(PrintType::kIFFT, frame_id);
                assert(frame_id == this->cur_proc_frame_id_);
                this->CheckIncrementScheduleFrame(frame_id, kDownlinkComplete);
                bool work_finished = this->CheckFrameComplete(frame_id);
                if (work_finished == true) {
                  goto finish;
                }
              }
            }
          }
        } break;

        case EventType::kPacketTX: {
          // Data is sent
          size_t ant_id = gen_tag_t(event.tags_[0]).ant_id_;
          size_t frame_id = gen_tag_t(event.tags_[0]).frame_id_;
          size_t symbol_id = gen_tag_t(event.tags_[0]).symbol_id_;
          PrintPerTaskDone(PrintType::kPacketTX, frame_id, symbol_id, ant_id);

          bool last_tx_task =
              this->tx_counters_.CompleteTask(frame_id, symbol_id);
          if (last_tx_task == true) {
            PrintPerSymbolDone(PrintType::kPacketTX, frame_id, symbol_id);
            // If tx of the first symbol is done
            if (symbol_id == cfg->Frame().GetDLSymbol(0)) {
              this->stats_->MasterSetTsc(TsType::kTXProcessedFirst, frame_id);
              PrintPerFrameDone(PrintType::kPacketTXFirst, frame_id);
            }

            bool last_tx_symbol = this->tx_counters_.CompleteSymbol(frame_id);
            if (last_tx_symbol == true) {
              this->stats_->MasterSetTsc(TsType::kTXDone, frame_id);
              PrintPerFrameDone(PrintType::kPacketTX, frame_id);

              bool work_finished = this->CheckFrameComplete(frame_id);
              if (work_finished == true) {
                goto finish;
              }
            }

            tx_count++;
            if (tx_count == tx_counters_.MaxSymbolCount() * 9000) {
              tx_count = 0;

              double diff = GetTime::GetTimeUs() - tx_begin;
              int samples_num_per_ue =
                  cfg->OfdmDataNum() * tx_counters_.MaxSymbolCount() * 1000;

              MLPD_INFO(
                  "TX %d samples (per-client) to %zu clients in %f secs, "
                  "throughtput %f bps per-client (16QAM), current tx queue "
                  "length %zu\n",
                  samples_num_per_ue, cfg->UeAntNum(), diff,
                  samples_num_per_ue * std::log2(16.0f) / diff,
                  GetConq(EventType::kPacketTX, 0)->size_approx());
              unused(diff);
              unused(samples_num_per_ue);
              tx_begin = GetTime::GetTimeUs();
            }
          }
        } break;
        default:
          MLPD_ERROR("Wrong event type in message queue!");
          std::exit(0);
      } /* End of switch */

      // We schedule FFT processing if the event handling above results in
      // either (a) sufficient packets received for the current frame,
      // or (b) the current frame being updated.
      std::queue<fft_req_tag_t>& cur_fftq =
          fft_queue_arr_[(this->cur_sche_frame_id_ % kFrameWnd)];
      size_t qid = this->cur_sche_frame_id_ & 0x1;
      if (cur_fftq.size() >= config_->FftBlockSize()) {
        size_t num_fft_blocks = cur_fftq.size() / config_->FftBlockSize();
        for (size_t i = 0; i < num_fft_blocks; i++) {
          EventData do_fft_task;
          do_fft_task.num_tags_ = config_->FftBlockSize();
          do_fft_task.event_type_ = EventType::kFFT;

          for (size_t j = 0; j < config_->FftBlockSize(); j++) {
            do_fft_task.tags_[j] = cur_fftq.front().tag_;
            cur_fftq.pop();

            if (this->fft_created_count_ == 0) {
              this->stats_->MasterSetTsc(TsType::kProcessingStarted,
                                         this->cur_sche_frame_id_);
            }
            this->fft_created_count_++;
            if (this->fft_created_count_ == rx_counters_.num_pkts_per_frame_) {
              this->fft_created_count_ = 0;
              if (cfg->BigstationMode() == true) {
                this->CheckIncrementScheduleFrame(cur_sche_frame_id_,
                                                  kUplinkComplete);
              }
            }
          }
          TryEnqueueFallback(GetConq(EventType::kFFT, qid),
                             GetPtok(EventType::kFFT, qid), do_fft_task);
        }
      }
    } /* End of for */
  }   /* End of while */

finish:
  MLPD_INFO("Agora: printing stats and saving to file\n");
  this->stats_->PrintSummary();
  this->stats_->SaveToFile();
  if (flags_.enable_save_decode_data_to_file_ == true) {
    SaveDecodeDataToFile(this->stats_->LastFrameId());
  }
  if (flags_.enable_save_tx_data_to_file_ == true) {
    SaveTxDataToFile(this->stats_->LastFrameId());
  }

  // Calculate and print per-user BER
  if ((kEnableMac == false) && (kPrintPhyStats == true)) {
    this->phy_stats_->PrintPhyStats();
  }
  this->Stop();
}

void Agora::HandleEventFft(size_t tag) {
  size_t frame_id = gen_tag_t(tag).frame_id_;
  size_t symbol_id = gen_tag_t(tag).symbol_id_;
  SymbolType sym_type = config_->GetSymbolType(symbol_id);

  if (sym_type == SymbolType::kPilot) {
    bool last_fft_task = pilot_fft_counters_.CompleteTask(frame_id, symbol_id);
    if (last_fft_task == true) {
      PrintPerSymbolDone(PrintType::kFFTPilots, frame_id, symbol_id);

      if ((config_->Frame().IsRecCalEnabled() == false) ||
          ((config_->Frame().IsRecCalEnabled() == true) &&
           (this->rc_last_frame_ == frame_id))) {
        // If CSI of all UEs is ready, schedule ZF/prediction
        bool last_pilot_fft = pilot_fft_counters_.CompleteSymbol(frame_id);
        if (last_pilot_fft == true) {
          this->stats_->MasterSetTsc(TsType::kFFTPilotsDone, frame_id);
          PrintPerFrameDone(PrintType::kFFTPilots, frame_id);
          this->pilot_fft_counters_.Reset(frame_id);
          if (kPrintPhyStats == true) {
            this->phy_stats_->PrintSnrStats(frame_id);
            if (config_->Frame().IsRecCalEnabled() == true) {
              size_t frame_grp_id =
                  (frame_id - TX_FRAME_DELTA) / config_->AntGroupNum();
              if ((frame_id - TX_FRAME_DELTA) % config_->AntGroupNum() == 0 &&
                  frame_grp_id > 0) {
                this->phy_stats_->PrintCalibSnrStats(frame_grp_id - 1);
              }
            }
          }
          if (kEnableMac == true) {
            SendSnrReport(EventType::kSNRReport, frame_id, symbol_id);
          }
          ScheduleSubcarriers(EventType::kZF, frame_id, 0);
        }
      }
    }
  } else if (sym_type == SymbolType::kUL) {
    size_t symbol_idx_ul = config_->Frame().GetULSymbolIdx(symbol_id);

    bool last_fft_per_symbol =
        uplink_fft_counters_.CompleteTask(frame_id, symbol_id);

    if (last_fft_per_symbol == true) {
      fft_cur_frame_for_symbol_.at(symbol_idx_ul) = frame_id;

      PrintPerSymbolDone(PrintType::kFFTData, frame_id, symbol_id);
      // If precoder exist, schedule demodulation
      if (zf_last_frame_ == frame_id) {
        ScheduleSubcarriers(EventType::kDemul, frame_id, symbol_id);
      }
      bool last_uplink_fft = uplink_fft_counters_.CompleteSymbol(frame_id);
      if (last_uplink_fft == true) {
        uplink_fft_counters_.Reset(frame_id);
      }
    }
  } else if ((sym_type == SymbolType::kCalDL) ||
             (sym_type == SymbolType::kCalUL)) {
    PrintPerSymbolDone(PrintType::kFFTCal, frame_id, symbol_id);

    bool last_rc_task = this->rc_counters_.CompleteTask(frame_id);
    if (last_rc_task == true) {
      PrintPerFrameDone(PrintType::kFFTCal, frame_id);
      this->rc_counters_.Reset(frame_id);
      this->stats_->MasterSetTsc(TsType::kRCDone, frame_id);
      this->rc_last_frame_ = frame_id;
    }
  }
}

void Agora::Worker(int tid) {
  PinToCoreWithOffset(ThreadType::kWorker, base_worker_core_offset_, tid);

  /* Initialize operators */
  auto compute_zf = std::make_unique<DoZF>(
      this->config_, tid, this->csi_buffers_, calib_dl_buffer_,
      calib_ul_buffer_, this->calib_dl_msum_buffer_,
      this->calib_ul_msum_buffer_, this->ul_zf_matrices_, this->dl_zf_matrices_,
      this->phy_stats_.get(), this->stats_.get());

  auto compute_fft = std::make_unique<DoFFT>(
      this->config_, tid, this->data_buffer_, this->csi_buffers_,
      this->calib_dl_buffer_, this->calib_ul_buffer_, this->phy_stats_.get(),
      this->stats_.get());

  // Downlink workers
  auto compute_ifft =
      std::make_unique<DoIFFT>(this->config_, tid, this->dl_ifft_buffer_,
                               this->dl_socket_buffer_, this->stats_.get());

  auto compute_precode = std::make_unique<DoPrecode>(
      this->config_, tid, this->dl_zf_matrices_, this->dl_ifft_buffer_,
      this->dl_encoded_buffer_, this->stats_.get());

  auto compute_encoding = std::make_unique<DoEncode>(
      config_, tid, Direction::kDownlink,
      (kEnableMac == true) ? dl_bits_buffer_ : config_->DlBits(),
      (kEnableMac == true) ? kFrameWnd : 1, dl_encoded_buffer_,
      this->stats_.get());

  // Uplink workers
  auto compute_decoding = std::make_unique<DoDecode>(
      this->config_, tid, this->demod_buffers_, this->decoded_buffer_,
      this->phy_stats_.get(), this->stats_.get());

  auto compute_demul = std::make_unique<DoDemul>(
      this->config_, tid, this->data_buffer_, this->ul_zf_matrices_,
      this->ue_spec_pilot_buffer_, this->equal_buffer_, this->demod_buffers_,
      this->phy_stats_.get(), this->stats_.get());

  std::vector<Doer*> computers_vec;
  std::vector<EventType> events_vec;
  ///*************************
  computers_vec.push_back(compute_zf.get());
  computers_vec.push_back(compute_fft.get());
  events_vec.push_back(EventType::kZF);
  events_vec.push_back(EventType::kFFT);

  if (config_->Frame().NumULSyms() > 0) {
    computers_vec.push_back(compute_decoding.get());
    computers_vec.push_back(compute_demul.get());
    events_vec.push_back(EventType::kDecode);
    events_vec.push_back(EventType::kDemul);
  }

  if (config_->Frame().NumDLSyms() > 0) {
    computers_vec.push_back(compute_ifft.get());
    computers_vec.push_back(compute_precode.get());
    computers_vec.push_back(compute_encoding.get());
    events_vec.push_back(EventType::kIFFT);
    events_vec.push_back(EventType::kPrecode);
    events_vec.push_back(EventType::kEncode);
  }

  size_t cur_qid = 0;
  size_t empty_queue_itrs = 0;
  bool empty_queue = true;
  while (this->config_->Running() == true) {
    for (size_t i = 0; i < computers_vec.size(); i++) {
      if (computers_vec.at(i)->TryLaunch(*GetConq(events_vec.at(i), cur_qid),
                                         complete_task_queue_[cur_qid],
                                         worker_ptoks_ptr_[tid][cur_qid])) {
        empty_queue = false;
        break;
      }
    }
    // If all queues in this set are empty for 5 iterations,
    // check the other set of queues
    if (empty_queue == true) {
      empty_queue_itrs++;
      if (empty_queue_itrs == 5) {
        if (this->cur_sche_frame_id_ != this->cur_proc_frame_id_) {
          cur_qid ^= 0x1;
        } else {
          cur_qid = (this->cur_sche_frame_id_ & 0x1);
        }
        empty_queue_itrs = 0;
      }
    } else {
      empty_queue = true;
    }
  }
  MLPD_SYMBOL("Agora worker %d exit\n", tid);
}

void Agora::WorkerFft(int tid) {
  PinToCoreWithOffset(ThreadType::kWorkerFFT, base_worker_core_offset_, tid);

  /* Initialize FFT operator */
  std::unique_ptr<DoFFT> compute_fft(
      new DoFFT(config_, tid, data_buffer_, csi_buffers_, calib_dl_buffer_,
                calib_ul_buffer_, this->phy_stats_.get(), this->stats_.get()));
  std::unique_ptr<DoIFFT> compute_ifft(new DoIFFT(
      config_, tid, dl_ifft_buffer_, dl_socket_buffer_, this->stats_.get()));

  while (this->config_->Running() == true) {
    // TODO refactor the if / else
    if (compute_fft->TryLaunch(*GetConq(EventType::kFFT, 0),
                               complete_task_queue_[0],
                               worker_ptoks_ptr_[tid][0]) == true) {
      // Do nothing
    } else if ((config_->Frame().NumDLSyms() > 0) &&
               (compute_ifft->TryLaunch(*GetConq(EventType::kIFFT, 0),
                                        complete_task_queue_[0],
                                        worker_ptoks_ptr_[tid][0]) == true)) {
      // Do nothing
    }
  }
}

void Agora::WorkerZf(int tid) {
  PinToCoreWithOffset(ThreadType::kWorkerZF, base_worker_core_offset_, tid);

  /* Initialize ZF operator */
  std::unique_ptr<DoZF> compute_zf(
      new DoZF(config_, tid, csi_buffers_, calib_dl_buffer_, calib_ul_buffer_,
               calib_dl_msum_buffer_, calib_ul_msum_buffer_, ul_zf_matrices_,
               dl_zf_matrices_, this->phy_stats_.get(), this->stats_.get()));

  while (this->config_->Running() == true) {
    compute_zf->TryLaunch(*GetConq(EventType::kZF, 0), complete_task_queue_[0],
                          worker_ptoks_ptr_[tid][0]);
  }
}

void Agora::WorkerDemul(int tid) {
  PinToCoreWithOffset(ThreadType::kWorkerDemul, base_worker_core_offset_, tid);

  std::unique_ptr<DoDemul> compute_demul(
      new DoDemul(config_, tid, data_buffer_, ul_zf_matrices_,
                  ue_spec_pilot_buffer_, equal_buffer_, demod_buffers_,
                  this->phy_stats_.get(), this->stats_.get()));

  /* Initialize Precode operator */
  std::unique_ptr<DoPrecode> compute_precode(
      new DoPrecode(config_, tid, dl_zf_matrices_, dl_ifft_buffer_,
                    dl_encoded_buffer_, this->stats_.get()));

  assert(false);

  while (this->config_->Running() == true) {
    if (config_->Frame().NumDLSyms() > 0) {
      compute_precode->TryLaunch(*GetConq(EventType::kDemul, 0),
                                 complete_task_queue_[0],
                                 worker_ptoks_ptr_[tid][0]);
    } else {
      compute_demul->TryLaunch(*GetConq(EventType::kPrecode, 0),
                               complete_task_queue_[0],
                               worker_ptoks_ptr_[tid][0]);
    }
  }
}

void Agora::WorkerDecode(int tid) {
  PinToCoreWithOffset(ThreadType::kWorkerDecode, base_worker_core_offset_, tid);

  std::unique_ptr<DoEncode> compute_encoding(
      new DoEncode(config_, tid, Direction::kDownlink,
                   (kEnableMac == true) ? dl_bits_buffer_ : config_->DlBits(),
                   (kEnableMac == true) ? kFrameWnd : 1, dl_encoded_buffer_,
                   this->stats_.get()));

  std::unique_ptr<DoDecode> compute_decoding(
      new DoDecode(config_, tid, demod_buffers_, decoded_buffer_,
                   this->phy_stats_.get(), this->stats_.get()));

  while (this->config_->Running() == true) {
    if (config_->Frame().NumDLSyms() > 0) {
      compute_encoding->TryLaunch(*GetConq(EventType::kEncode, 0),
                                  complete_task_queue_[0],
                                  worker_ptoks_ptr_[tid][0]);
    } else {
      compute_decoding->TryLaunch(*GetConq(EventType::kDecode, 0),
                                  complete_task_queue_[0],
                                  worker_ptoks_ptr_[tid][0]);
    }
  }
}

void Agora::CreateThreads() {
  const auto& cfg = config_;
  if (cfg->BigstationMode() == true) {
    for (size_t i = 0; i < cfg->FftThreadNum(); i++) {
      workers_.emplace_back(&Agora::WorkerFft, this, i);
    }
    for (size_t i = cfg->FftThreadNum();
         i < cfg->FftThreadNum() + cfg->ZfThreadNum(); i++) {
      workers_.emplace_back(&Agora::WorkerZf, this, i);
    }
    for (size_t i = cfg->FftThreadNum() + cfg->ZfThreadNum();
         i < cfg->FftThreadNum() + cfg->ZfThreadNum() + cfg->DemulThreadNum();
         i++) {
      workers_.emplace_back(&Agora::WorkerDemul, this, i);
    }
    for (size_t i =
             cfg->FftThreadNum() + cfg->ZfThreadNum() + cfg->DemulThreadNum();
         i < cfg->WorkerThreadNum(); i++) {
      workers_.emplace_back(&Agora::WorkerDecode, this, i);
    }
  } else {
    MLPD_SYMBOL("Agora: creating %zu workers\n", cfg->WorkerThreadNum());
    for (size_t i = 0; i < cfg->WorkerThreadNum(); i++) {
      workers_.emplace_back(&Agora::Worker, this, i);
    }
  }
}

void Agora::UpdateRanConfig(RanConfig rc) {
  config_->UpdateModCfgs(rc.mod_order_bits_);
}

void Agora::UpdateRxCounters(size_t frame_id, size_t symbol_id) {
  const size_t frame_slot = frame_id % kFrameWnd;
  if (config_->IsPilot(frame_id, symbol_id)) {
    rx_counters_.num_pilot_pkts_[frame_slot]++;
    if (rx_counters_.num_pilot_pkts_[frame_slot] ==
        rx_counters_.num_pilot_pkts_per_frame_) {
      rx_counters_.num_pilot_pkts_[frame_slot] = 0;
      this->stats_->MasterSetTsc(TsType::kPilotAllRX, frame_id);
      PrintPerFrameDone(PrintType::kPacketRXPilots, frame_id);
    }
  } else if (config_->IsCalDlPilot(frame_id, symbol_id) or
             config_->IsCalUlPilot(frame_id, symbol_id)) {
    if (++rx_counters_.num_reciprocity_pkts_[frame_slot] ==
        rx_counters_.num_reciprocity_pkts_per_frame_) {
      rx_counters_.num_reciprocity_pkts_[frame_slot] = 0;
      this->stats_->MasterSetTsc(TsType::kRCAllRX, frame_id);
    }
  }
  // Receive first packet in a frame
  if (rx_counters_.num_pkts_[frame_slot] == 0) {
    if (kEnableMac == false) {
      // schedule this frame's encoding
      // Defer the schedule.  If frames are already deferred or the current
      // received frame is too far off
      if ((this->encode_deferral_.empty() == false) ||
          (frame_id >= (this->cur_proc_frame_id_ + kScheduleQueues))) {
        if (kDebugDeferral) {
          std::printf("   +++ Deferring encoding of frame %zu\n", frame_id);
        }
        this->encode_deferral_.push(frame_id);
      } else {
        ScheduleDownlinkProcessing(frame_id);
      }
    }
    this->stats_->MasterSetTsc(TsType::kFirstSymbolRX, frame_id);
    if (kDebugPrintPerFrameStart) {
      const size_t prev_frame_slot = (frame_slot + kFrameWnd - 1) % kFrameWnd;
      std::printf(
          "Main [frame %zu + %.2f ms since last frame]: Received "
          "first packet. Remaining packets in prev frame: %zu\n",
          frame_id,
          this->stats_->MasterGetDeltaMs(TsType::kFirstSymbolRX, frame_id,
                                         frame_id - 1),
          rx_counters_.num_pkts_[prev_frame_slot]);
    }
  }

  rx_counters_.num_pkts_[frame_slot]++;
  if (rx_counters_.num_pkts_[frame_slot] == rx_counters_.num_pkts_per_frame_) {
    this->stats_->MasterSetTsc(TsType::kRXDone, frame_id);
    PrintPerFrameDone(PrintType::kPacketRX, frame_id);
    rx_counters_.num_pkts_[frame_slot] = 0;
  }
}

void Agora::PrintPerFrameDone(PrintType print_type, size_t frame_id) {
  if (kDebugPrintPerFrameDone == true) {
    switch (print_type) {
      case (PrintType::kPacketRXPilots):
        std::printf("Main [frame %zu + %.2f ms]: Received all pilots\n",
                    frame_id,
                    this->stats_->MasterGetDeltaMs(
                        TsType::kPilotAllRX, TsType::kFirstSymbolRX, frame_id));
        break;
      case (PrintType::kPacketRX):
        std::printf("Main [frame %zu + %.2f ms]: Received all packets\n",
                    frame_id,
                    this->stats_->MasterGetDeltaMs(
                        TsType::kRXDone, TsType::kFirstSymbolRX, frame_id));
        break;
      case (PrintType::kFFTPilots):
        std::printf(
            "Main [frame %zu + %.2f ms]: FFT-ed all pilots\n", frame_id,
            this->stats_->MasterGetDeltaMs(TsType::kFFTPilotsDone,
                                           TsType::kFirstSymbolRX, frame_id));
        break;
      case (PrintType::kFFTCal):
        std::printf(
            "Main [frame %zu + %.2f ms]: FFT-ed all calibration symbols\n",
            frame_id,
            this->stats_->MasterGetUsSince(TsType::kRCAllRX, frame_id) /
                1000.0);
        break;
      case (PrintType::kZF):
        std::printf("Main [frame %zu + %.2f ms]: Completed zero-forcing\n",
                    frame_id,
                    this->stats_->MasterGetDeltaMs(
                        TsType::kZFDone, TsType::kFirstSymbolRX, frame_id));
        break;
      case (PrintType::kDemul):
        std::printf("Main [frame %zu + %.2f ms]: Completed demodulation\n",
                    frame_id,
                    this->stats_->MasterGetDeltaMs(
                        TsType::kDemulDone, TsType::kFirstSymbolRX, frame_id));
        break;
      case (PrintType::kDecode):
        std::printf(
            "Main [frame %zu + %.2f ms]: Completed LDPC decoding (%zu UL "
            "symbols)\n",
            frame_id,
            this->stats_->MasterGetDeltaMs(TsType::kDecodeDone,
                                           TsType::kFirstSymbolRX, frame_id),
            config_->Frame().NumULSyms());
        break;
      case (PrintType::kPacketFromMac):
        std::printf(
            "Main [frame %zu + %.2f ms]: Completed MAC RX \n", frame_id,
            this->stats_->MasterGetMsSince(TsType::kFirstSymbolRX, frame_id));
        break;
      case (PrintType::kEncode):
        std::printf("Main [frame %zu + %.2f ms]: Completed LDPC encoding\n",
                    frame_id,
                    this->stats_->MasterGetDeltaMs(
                        TsType::kEncodeDone, TsType::kFirstSymbolRX, frame_id));
        break;
      case (PrintType::kPrecode):
        std::printf(
            "Main [frame %zu + %.2f ms]: Completed precoding\n", frame_id,
            this->stats_->MasterGetDeltaMs(TsType::kPrecodeDone,
                                           TsType::kFirstSymbolRX, frame_id));
        break;
      case (PrintType::kIFFT):
        std::printf("Main [frame %zu + %.2f ms]: Completed IFFT\n", frame_id,
                    this->stats_->MasterGetDeltaMs(
                        TsType::kIFFTDone, TsType::kFirstSymbolRX, frame_id));
        break;
      case (PrintType::kPacketTXFirst):
        std::printf(
            "Main [frame %zu + %.2f ms]: Completed TX of first symbol\n",
            frame_id,
            this->stats_->MasterGetDeltaMs(TsType::kTXProcessedFirst,
                                           TsType::kFirstSymbolRX, frame_id));
        break;
      case (PrintType::kPacketTX):
        std::printf(
            "Main [frame %zu + %.2f ms]: Completed TX (%zu DL symbols)\n",
            frame_id,
            this->stats_->MasterGetDeltaMs(TsType::kTXDone,
                                           TsType::kFirstSymbolRX, frame_id),
            config_->Frame().NumDLSyms());
        break;
      case (PrintType::kPacketToMac):
        std::printf(
            "Main [frame %zu + %.2f ms]: Completed MAC TX \n", frame_id,
            this->stats_->MasterGetMsSince(TsType::kFirstSymbolRX, frame_id));
        break;
      default:
        std::printf("Wrong task type in frame done print!");
    }
  }
}

void Agora::PrintPerSymbolDone(PrintType print_type, size_t frame_id,
                               size_t symbol_id) {
  if (kDebugPrintPerSymbolDone == true) {
    switch (print_type) {
      case (PrintType::kFFTPilots):
        std::printf(
            "Main [frame %zu symbol %zu + %.3f ms]: FFT-ed pilot symbol, "
            "%zu symbols done\n",
            frame_id, symbol_id,
            this->stats_->MasterGetMsSince(TsType::kFirstSymbolRX, frame_id),
            pilot_fft_counters_.GetSymbolCount(frame_id) + 1);
        break;
      case (PrintType::kFFTData):
        std::printf(
            "Main [frame %zu symbol %zu + %.3f ms]: FFT-ed data symbol, "
            "%zu precoder status: %d\n",
            frame_id, symbol_id,
            this->stats_->MasterGetMsSince(TsType::kFirstSymbolRX, frame_id),
            uplink_fft_counters_.GetSymbolCount(frame_id) + 1,
            static_cast<int>(zf_last_frame_ == frame_id));
        break;
      case (PrintType::kDemul):
        std::printf(
            "Main [frame %zu symbol %zu + %.3f ms]: Completed "
            "demodulation, "
            "%zu symbols done\n",
            frame_id, symbol_id,
            this->stats_->MasterGetMsSince(TsType::kFirstSymbolRX, frame_id),
            demul_counters_.GetSymbolCount(frame_id) + 1);
        break;
      case (PrintType::kDecode):
        std::printf(
            "Main [frame %zu symbol %zu + %.3f ms]: Completed decoding, "
            "%zu symbols done\n",
            frame_id, symbol_id,
            this->stats_->MasterGetMsSince(TsType::kFirstSymbolRX, frame_id),
            decode_counters_.GetSymbolCount(frame_id) + 1);
        break;
      case (PrintType::kEncode):
        std::printf(
            "Main [frame %zu symbol %zu + %.3f ms]: Completed encoding, "
            "%zu symbols done\n",
            frame_id, symbol_id,
            this->stats_->MasterGetMsSince(TsType::kFirstSymbolRX, frame_id),
            encode_counters_.GetSymbolCount(frame_id) + 1);
        break;
      case (PrintType::kPrecode):
        std::printf(
            "Main [frame %zu symbol %zu + %.3f ms]: Completed precoding, "
            "%zu symbols done\n",
            frame_id, symbol_id,
            this->stats_->MasterGetMsSince(TsType::kFirstSymbolRX, frame_id),
            precode_counters_.GetSymbolCount(frame_id) + 1);
        break;
      case (PrintType::kIFFT):
        std::printf(
            "Main [frame %zu symbol %zu + %.3f ms]: Completed IFFT, "
            "%zu symbols done\n",
            frame_id, symbol_id,
            this->stats_->MasterGetMsSince(TsType::kFirstSymbolRX, frame_id),
            ifft_counters_.GetSymbolCount(frame_id) + 1);
        break;
      case (PrintType::kPacketTX):
        std::printf(
            "Main [frame %zu symbol %zu + %.3f ms]: Completed TX, "
            "%zu symbols done\n",
            frame_id, symbol_id,
            this->stats_->MasterGetMsSince(TsType::kFirstSymbolRX, frame_id),
            tx_counters_.GetSymbolCount(frame_id) + 1);
        break;
      case (PrintType::kPacketToMac):
        std::printf(
            "Main [frame %zu symbol %zu + %.3f ms]: Completed MAC TX, "
            "%zu symbols done\n",
            frame_id, symbol_id,
            this->stats_->MasterGetMsSince(TsType::kFirstSymbolRX, frame_id),
            tomac_counters_.GetSymbolCount(frame_id) + 1);
        break;
      default:
        std::printf("Wrong task type in symbol done print!");
    }
  }
}

void Agora::PrintPerTaskDone(PrintType print_type, size_t frame_id,
                             size_t symbol_id, size_t ant_or_sc_id) {
  if (kDebugPrintPerTaskDone == true) {
    switch (print_type) {
      case (PrintType::kZF):
        std::printf("Main thread: ZF done frame: %zu, subcarrier %zu\n",
                    frame_id, ant_or_sc_id);
        break;
      case (PrintType::kRC):
        std::printf("Main thread: RC done frame: %zu, subcarrier %zu\n",
                    frame_id, ant_or_sc_id);
        break;
      case (PrintType::kDemul):
        std::printf(
            "Main thread: Demodulation done frame: %zu, symbol: %zu, sc: "
            "%zu, num blocks done: %zu\n",
            frame_id, symbol_id, ant_or_sc_id,
            demul_counters_.GetTaskCount(frame_id, symbol_id));
        break;
      case (PrintType::kDecode):
        std::printf(
            "Main thread: Decoding done frame: %zu, symbol: %zu, sc: %zu, "
            "num blocks done: %zu\n",
            frame_id, symbol_id, ant_or_sc_id,
            decode_counters_.GetTaskCount(frame_id, symbol_id));
        break;
      case (PrintType::kPrecode):
        std::printf(
            "Main thread: Precoding done frame: %zu, symbol: %zu, "
            "subcarrier: %zu, total SCs: %zu\n",
            frame_id, symbol_id, ant_or_sc_id,
            precode_counters_.GetTaskCount(frame_id, symbol_id));
        break;
      case (PrintType::kIFFT):
        std::printf(
            "Main thread: IFFT done frame: %zu, symbol: %zu, antenna: %zu, "
            "total ants: %zu\n",
            frame_id, symbol_id, ant_or_sc_id,
            ifft_counters_.GetTaskCount(frame_id, symbol_id));
        break;
      case (PrintType::kPacketTX):
        std::printf(
            "Main thread: TX done frame: %zu, symbol: %zu, antenna: %zu, "
            "total packets: %zu\n",
            frame_id, symbol_id, ant_or_sc_id,
            tx_counters_.GetTaskCount(frame_id, symbol_id));
        break;
      default:
        std::printf("Wrong task type in task done print!");
    }
  }
}

void Agora::InitializeQueues() {
  using mt_queue_t = moodycamel::ConcurrentQueue<EventData>;

  int data_symbol_num_perframe = config_->Frame().NumDataSyms();
  message_queue_ =
      mt_queue_t(kDefaultMessageQueueSize * data_symbol_num_perframe);
  for (auto& c : complete_task_queue_) {
    c = mt_queue_t(kDefaultWorkerQueueSize * data_symbol_num_perframe);
  }
  // Create concurrent queues for each Doer
  for (auto& vec : sched_info_arr_) {
    for (auto& s : vec) {
      s.concurrent_q_ =
          mt_queue_t(kDefaultWorkerQueueSize * data_symbol_num_perframe);
      s.ptok_ = new moodycamel::ProducerToken(s.concurrent_q_);
    }
  }

  for (size_t i = 0; i < config_->SocketThreadNum(); i++) {
    rx_ptoks_ptr_[i] = new moodycamel::ProducerToken(message_queue_);
    tx_ptoks_ptr_[i] =
        new moodycamel::ProducerToken(*GetConq(EventType::kPacketTX, 0));
  }

  for (size_t i = 0; i < config_->WorkerThreadNum(); i++) {
    for (size_t j = 0; j < kScheduleQueues; j++) {
      worker_ptoks_ptr_[i][j] =
          new moodycamel::ProducerToken(complete_task_queue_[j]);
    }
  }
}

void Agora::FreeQueues() {
  // remove tokens for each doer
  for (auto& vec : sched_info_arr_) {
    for (auto& s : vec) {
      delete s.ptok_;
    }
  }

  for (size_t i = 0; i < config_->SocketThreadNum(); i++) {
    delete rx_ptoks_ptr_[i];
    delete tx_ptoks_ptr_[i];
  }

  for (size_t i = 0; i < config_->WorkerThreadNum(); i++) {
    for (size_t j = 0; j < kScheduleQueues; j++) {
      delete worker_ptoks_ptr_[i][j];
    }
  }
}

void Agora::InitializeUplinkBuffers() {
  const auto& cfg = config_;
  const size_t task_buffer_symbol_num_ul = cfg->Frame().NumULSyms() * kFrameWnd;

  socket_buffer_size_ = cfg->PacketLength() * cfg->BsAntNum() * kFrameWnd *
                        cfg->Frame().NumTotalSyms();

  socket_buffer_.Malloc(cfg->SocketThreadNum() /* RX */, socket_buffer_size_,
                        Agora_memory::Alignment_t::kAlign64);

  data_buffer_.Malloc(task_buffer_symbol_num_ul,
                      cfg->OfdmDataNum() * cfg->BsAntNum(),
                      Agora_memory::Alignment_t::kAlign64);

  equal_buffer_.Malloc(task_buffer_symbol_num_ul,
                       cfg->OfdmDataNum() * cfg->UeAntNum(),
                       Agora_memory::Alignment_t::kAlign64);
  ue_spec_pilot_buffer_.Calloc(
      kFrameWnd, cfg->Frame().ClientUlPilotSymbols() * cfg->UeAntNum(),
      Agora_memory::Alignment_t::kAlign64);

  rx_counters_.num_pkts_per_frame_ =
      cfg->BsAntNum() *
      (cfg->Frame().NumPilotSyms() + cfg->Frame().NumULSyms() +
       static_cast<size_t>(cfg->Frame().IsRecCalEnabled()));
  rx_counters_.num_pilot_pkts_per_frame_ =
      cfg->BsAntNum() * cfg->Frame().NumPilotSyms();
  rx_counters_.num_reciprocity_pkts_per_frame_ = cfg->BsAntNum();

  fft_created_count_ = 0;
  pilot_fft_counters_.Init(cfg->Frame().NumPilotSyms(), cfg->BsAntNum());
  uplink_fft_counters_.Init(cfg->Frame().NumULSyms(), cfg->BsAntNum());
  fft_cur_frame_for_symbol_ =
      std::vector<size_t>(cfg->Frame().NumULSyms(), SIZE_MAX);

  rc_counters_.Init(cfg->BsAntNum());

  zf_counters_.Init(cfg->ZfEventsPerSymbol());

  demul_counters_.Init(cfg->Frame().NumULSyms(), cfg->DemulEventsPerSymbol());

  decode_counters_.Init(
      cfg->Frame().NumULSyms(),
      cfg->LdpcConfig().NumBlocksInSymbol() * cfg->UeAntNum());

  tomac_counters_.Init(cfg->Frame().NumULSyms(), cfg->UeAntNum());
}

void Agora::InitializeDownlinkBuffers() {
  if (config_->Frame().NumDLSyms() > 0) {
    std::printf("Agora: Initializing downlink buffers\n");

    const size_t task_buffer_symbol_num =
        config_->Frame().NumDLSyms() * kFrameWnd;

    size_t dl_socket_buffer_status_size =
        config_->BsAntNum() * task_buffer_symbol_num;
    size_t dl_socket_buffer_size =
        config_->DlPacketLength() * dl_socket_buffer_status_size;
    AllocBuffer1d(&dl_socket_buffer_, dl_socket_buffer_size,
                  Agora_memory::Alignment_t::kAlign64, 0);
    AllocBuffer1d(&dl_socket_buffer_status_, dl_socket_buffer_status_size,
                  Agora_memory::Alignment_t::kAlign64, 1);

    size_t dl_bits_buffer_size = kFrameWnd * config_->DlMacBytesNumPerframe();
    this->dl_bits_buffer_.Calloc(config_->UeAntNum(), dl_bits_buffer_size,
                                 Agora_memory::Alignment_t::kAlign64);
    this->dl_bits_buffer_status_.Calloc(config_->UeAntNum(), kFrameWnd,
                                        Agora_memory::Alignment_t::kAlign64);

    dl_ifft_buffer_.Calloc(config_->BsAntNum() * task_buffer_symbol_num,
                           config_->OfdmCaNum(),
                           Agora_memory::Alignment_t::kAlign64);
    calib_dl_buffer_.Calloc(kFrameWnd,
                            config_->BfAntNum() * config_->OfdmDataNum(),
                            Agora_memory::Alignment_t::kAlign64);
    calib_ul_buffer_.Calloc(kFrameWnd,
                            config_->BfAntNum() * config_->OfdmDataNum(),
                            Agora_memory::Alignment_t::kAlign64);
    calib_dl_msum_buffer_.Calloc(kFrameWnd,
                                 config_->BfAntNum() * config_->OfdmDataNum(),
                                 Agora_memory::Alignment_t::kAlign64);
    calib_ul_msum_buffer_.Calloc(kFrameWnd,
                                 config_->BfAntNum() * config_->OfdmDataNum(),
                                 Agora_memory::Alignment_t::kAlign64);
    // initialize the content of the last window to 1
    for (size_t i = 0; i < config_->OfdmDataNum() * config_->BfAntNum(); i++) {
      calib_dl_buffer_[kFrameWnd - 1][i] = {1, 0};
      calib_ul_buffer_[kFrameWnd - 1][i] = {1, 0};
    }
    dl_encoded_buffer_.Calloc(
        task_buffer_symbol_num,
        Roundup<64>(config_->OfdmDataNum()) * config_->UeAntNum(),
        Agora_memory::Alignment_t::kAlign64);

    encode_counters_.Init(
        config_->Frame().NumDlDataSyms(),
        config_->LdpcConfig().NumBlocksInSymbol() * config_->UeAntNum());
    encode_cur_frame_for_symbol_ =
        std::vector<size_t>(config_->Frame().NumDLSyms(), SIZE_MAX);
    ifft_cur_frame_for_symbol_ =
        std::vector<size_t>(config_->Frame().NumDLSyms(), SIZE_MAX);
    precode_counters_.Init(config_->Frame().NumDLSyms(),
                           config_->DemulEventsPerSymbol());
    // precode_cur_frame_for_symbol_ =
    //    std::vector<size_t>(config_->Frame().NumDLSyms(), SIZE_MAX);
    ifft_counters_.Init(config_->Frame().NumDLSyms(), config_->BsAntNum());
    tx_counters_.Init(config_->Frame().NumDLSyms(), config_->BsAntNum());
    // mac data is sent per frame, so we set max symbol to 1
    mac_to_phy_counters_.Init(1, config_->UeAntNum());
  }
}

void Agora::FreeUplinkBuffers() {
  socket_buffer_.Free();
  data_buffer_.Free();
  equal_buffer_.Free();
  ue_spec_pilot_buffer_.Free();
}

void Agora::FreeDownlinkBuffers() {
  if (config_->Frame().NumDLSyms() > 0) {
    FreeBuffer1d(&dl_socket_buffer_);
    FreeBuffer1d(&dl_socket_buffer_status_);

    dl_ifft_buffer_.Free();
    calib_dl_buffer_.Free();
    calib_ul_buffer_.Free();
    calib_dl_msum_buffer_.Free();
    calib_ul_msum_buffer_.Free();
    dl_encoded_buffer_.Free();
    dl_bits_buffer_.Free();
    dl_bits_buffer_status_.Free();
  }
}

void Agora::SaveDecodeDataToFile(int frame_id) {
  const auto& cfg = config_;
  const size_t num_decoded_bytes =
      cfg->NumBytesPerCb() * cfg->LdpcConfig().NumBlocksInSymbol();

  std::string cur_directory = TOSTRING(PROJECT_DIRECTORY);
  std::string filename = cur_directory + "/data/decode_data.bin";
  std::printf("Saving decode data to %s\n", filename.c_str());
  FILE* fp = std::fopen(filename.c_str(), "wb");

  for (size_t i = 0; i < cfg->Frame().NumULSyms(); i++) {
    for (size_t j = 0; j < cfg->UeAntNum(); j++) {
      int8_t* ptr = decoded_buffer_[(frame_id % kFrameWnd)][i][j];
      std::fwrite(ptr, num_decoded_bytes, sizeof(uint8_t), fp);
    }
  }
  std::fclose(fp);
}

void Agora::SaveTxDataToFile(UNUSED int frame_id) {
  const auto& cfg = config_;

  std::string cur_directory = TOSTRING(PROJECT_DIRECTORY);
  std::string filename = cur_directory + "/data/tx_data.bin";
  std::printf("Saving Frame %d TX data to %s\n", frame_id, filename.c_str());
  FILE* fp = std::fopen(filename.c_str(), "wb");

  for (size_t i = 0; i < cfg->Frame().NumDLSyms(); i++) {
    size_t total_data_symbol_id = cfg->GetTotalDataSymbolIdxDl(frame_id, i);

    for (size_t ant_id = 0; ant_id < cfg->BsAntNum(); ant_id++) {
      size_t offset = total_data_symbol_id * cfg->BsAntNum() + ant_id;
      auto* pkt = reinterpret_cast<struct Packet*>(
          &dl_socket_buffer_[offset * cfg->DlPacketLength()]);
      short* socket_ptr = pkt->data_;
      std::fwrite(socket_ptr, cfg->SampsPerSymbol() * 2, sizeof(short), fp);
    }
  }
  std::fclose(fp);
}

void Agora::GetEqualData(float** ptr, int* size) {
  const auto& cfg = config_;
  auto offset = cfg->GetTotalDataSymbolIdxUl(
      max_equaled_frame_, cfg->Frame().ClientUlPilotSymbols());
  *ptr = (float*)&equal_buffer_[offset][0];
  *size = cfg->UeAntNum() * cfg->OfdmDataNum() * 2;
}
void Agora::CheckIncrementScheduleFrame(size_t frame_id,
                                        ScheduleProcessingFlags completed) {
  this->schedule_process_flags_ += completed;
  assert(this->cur_sche_frame_id_ == frame_id);
  unused(frame_id);

  if (this->schedule_process_flags_ ==
      static_cast<uint8_t>(ScheduleProcessingFlags::kProcessingComplete)) {
    this->cur_sche_frame_id_++;
    this->schedule_process_flags_ = ScheduleProcessingFlags::kNone;
    if (this->config_->Frame().NumULSyms() == 0) {
      this->schedule_process_flags_ += ScheduleProcessingFlags::kUplinkComplete;
    }
    if (this->config_->Frame().NumDLSyms() == 0) {
      this->schedule_process_flags_ +=
          ScheduleProcessingFlags::kDownlinkComplete;
    }
  }
}

bool Agora::CheckFrameComplete(size_t frame_id) {
  bool finished = false;

  MLPD_TRACE(
      "Checking work complete %zu, ifft %d, tx %d, decode %d, tomac %d, tx "
      "%d\n",
      frame_id, static_cast<int>(this->ifft_counters_.IsLastSymbol(frame_id)),
      static_cast<int>(this->tx_counters_.IsLastSymbol(frame_id)),
      static_cast<int>(this->decode_counters_.IsLastSymbol(frame_id)),
      static_cast<int>(this->tomac_counters_.IsLastSymbol(frame_id)),
      static_cast<int>(this->tx_counters_.IsLastSymbol(frame_id)));

  // Complete if last frame and ifft / decode complete
  if ((true == this->ifft_counters_.IsLastSymbol(frame_id)) &&
      (true == this->tx_counters_.IsLastSymbol(frame_id)) &&
      (((false == kEnableMac) &&
        (true == this->decode_counters_.IsLastSymbol(frame_id))) ||
       ((true == kEnableMac) &&
        (true == this->tomac_counters_.IsLastSymbol(frame_id))))) {
    this->stats_->UpdateStats(frame_id);
    assert(frame_id == this->cur_proc_frame_id_);
    this->decode_counters_.Reset(frame_id);
    this->tomac_counters_.Reset(frame_id);
    this->ifft_counters_.Reset(frame_id);
    this->tx_counters_.Reset(frame_id);
    if (config_->Frame().NumDLSyms() > 0) {
      for (size_t ue_id = 0; ue_id < config_->UeAntNum(); ue_id++) {
        this->dl_bits_buffer_status_[ue_id][frame_id % kFrameWnd] = 0;
      }
    }
    this->cur_proc_frame_id_++;

    if (this->encode_deferral_.empty() == false) {
      for (size_t encode = 0; encode < kScheduleQueues; encode++) {
        const size_t deferred_frame = this->encode_deferral_.front();
        if (deferred_frame < (this->cur_proc_frame_id_ + kScheduleQueues)) {
          if (kDebugDeferral) {
            std::printf("   +++ Scheduling deferred frame %zu : %zu \n",
                        deferred_frame, cur_proc_frame_id_);
          }
          RtAssert(deferred_frame >= this->cur_proc_frame_id_,
                   "Error scheduling encoding because deferral frame is less "
                   "than current frame");
          ScheduleDownlinkProcessing(deferred_frame);
          this->encode_deferral_.pop();
        } else {
          // No need to check the next frame because it is too large
          break;
        }
      }
    }

    if (frame_id == (this->config_->FramesToTest() - 1)) {
      finished = true;
    }
  }
  return finished;
}

extern "C" {
EXPORT Agora* AgoraNew(Config* cfg) {
  // std::printf("Size of Agora: %d\n",sizeof(Agora *));
  auto* agora = new Agora(cfg);

  return agora;
}
EXPORT void AgoraStart(Agora* agora) { agora->Start(); }
EXPORT void AgoraStop(/*Agora *agora*/) {
  SignalHandler::SetExitSignal(true); /*agora->stop();*/
}
EXPORT void AgoraDestroy(Agora* agora) { delete agora; }
EXPORT void AgoraGetEqualData(Agora* agora, float** ptr, int* size) {
  return agora->GetEqualData(ptr, size);
}
}
