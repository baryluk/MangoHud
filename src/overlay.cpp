#include <sstream>
#include <iomanip>
#include <algorithm>
#include "overlay.h"
#include "logging.h"
#include "cpu.h"
#include "gpu.h"
#include "memory.h"
#include "timing.hpp"
#include "mesa/util/macros.h"
#include "string_utils.h"

#include "server/common.h"

// getpid, getuid
#include <sys/types.h>
#include <unistd.h>


#ifdef HAVE_DBUS
float g_overflow = 50.f /* 3333ms * 0.5 / 16.6667 / 2 (to edge and back) */;
#endif

bool open = false;
struct benchmark_stats benchmark;
struct fps_limit fps_limit_stats {};
ImVec2 real_font_size;
std::vector<logData> graph_data;

extern std::string program_name;

static int mango_message_generator(Message* message, void* my_state) {
   assert(message != NULL);
   assert(my_state != NULL);

   const uint64_t now = os_time_get(); /* us */

   struct swapchain_stats *const sw_stats = (struct swapchain_stats *)my_state;

   PB_MALLOC_SET(message->protocol_version, 1);
   PB_MALLOC_SET(message->client_type, Message_ClientType_APP);
   PB_MALLOC_SET(message->pid, getpid());
   PB_MALLOC_SET(message->uid, getuid());
   PB_MALLOC_SET(message->fps, fps);
   if (program_name.size()) {
       message->program_name = strndup(program_name.data(), program_name.length());
   }
   if (wine_version.size()) {
       message->wine_version = strndup(wine_version.data(), wine_version.length());
   }

   PB_MALLOC_SET(message->timestamp, Timestamp_init_zero);
   PB_MALLOC_SET(message->timestamp->clock_source, TimestampSource_MONOTONIC);
   PB_MALLOC_SET(message->timestamp->timestamp, now);

   // PB_MALLOC_SET(message->app_uptime_msec, );  // In Linux there is no way to really find this out.
   // A possible option might be reading /proc/self/sched
   // and reading se.exec_start                                :     785249279.391105
   // We can do it once.
   // Not sure which clock it is using. Definitively not any of REALTIME ones.

/*
  uint64 app_uptime_msec = 21;

  repeated FrameTime frametimes = 31;

  bool stream_frametimes = 40;

  bool show_hud = 41;

  repeated float frame_limits = 42;  // Can be a list of frame limits a user can toggle between, or empty to disable.
  bool vsync = 43;
*/

/*

message FrameTime {
  // All in microseconds.
  uint64 timestamp = 1;  // Absolute timestamp. Monotonic.
  uint64 index = 2;  // Counter of frames. Monotonic.
  uint32 time = 3;  // The actual frametime.
}
*/

   PB_MALLOC_SET(message->render_info, RenderInfo_init_zero);
   RenderInfo *const render_info = message->render_info;
   if (sw_stats->version_gl.major && sw_stats->version_gl.minor) {
     PB_MALLOC_SET(render_info->opengl, true);
     PB_MALLOC_SET(render_info->version_gl_major, sw_stats->version_gl.major);
     PB_MALLOC_SET(render_info->version_gl_minor, sw_stats->version_gl.minor);
     PB_MALLOC_SET(render_info->version_gl_is_gles, sw_stats->version_gl.is_gles); // bool

     // vendor string
     // renderer string
     // core profile version string:
     // Video memory
   }
   if (sw_stats->version_vk.major && sw_stats->version_vk.minor) {
     PB_MALLOC_SET(render_info->vulkan, true);
     PB_MALLOC_SET(render_info->version_vk_major, sw_stats->version_vk.major);
     PB_MALLOC_SET(render_info->version_vk_minor, sw_stats->version_vk.minor);
     PB_MALLOC_SET(render_info->version_vk_patch, sw_stats->version_vk.patch);
   }
   render_info->engine_name = strndup(sw_stats->engineName.data(), sw_stats->engineName.length());
   render_info->engine_version = strndup(sw_stats->engineVersion.data(), sw_stats->engineVersion.length());
   render_info->device_name = strndup(sw_stats->deviceName.data(), sw_stats->deviceName.length());
   render_info->gpu_name = strndup(sw_stats->gpuName.data(), sw_stats->gpuName.length());
   render_info->driver_name = strndup(sw_stats->driverName.data(), sw_stats->driverName.length());
   PB_MALLOC_SET(render_info_>device_id, deviceID); // int32


   PB_MALLOC_SET(message->frames, sw_stats->n_frames);  // uint64_t
   PB_MALLOC_SET(message->last_present_time_usec, sw_stats->last_present_time);  // uint64_t
   PB_MALLOC_SET(message->frames_since_update, sw_stats->n_frames_since_update);  // unsigned
   PB_MALLOC_SET(message->last_fps_update_usec, sw_stats->last_fps_update);  // uint64_t
   // PB_MALLOC_SET(message->hud->main_window_pos_x, sw_stats->main_window_pos.x);  // ImVec2
   // PB_MALLOC_SET(message->hud->main_window_pos_y, sw_stats->main_window_pos.y);


   // params.fps_sampling_period


   // System / Process IO.
   if (true) {
     //PB_MALLOC_SET(message->app_io_info, AppIoInfo_init_zero);
     //AppIoInfo *const app_io = message->app_io_info;
     //IoInfo *const sys_io = message->io_info;

//   struct iostats io;

     // Rates (computed on client side)
     // PB_MALLOC_SET(app_io, sw_stats->io.read_iops);

     // Cummulatives
     // PB_MALLOC_SET(app_io, sw_stats->io.read_iops);
   }

   if (

   // Frame stats
   // double time_dividor;
   // struct frame_stat stats_min, stats_max;

   // All frametime dumps
/*
struct frame_stat {
   uint64_t stats[OVERLAY_PLOTS_MAX];
};
*/

//   struct frame_stat frames_stats[200];


// More stuff to copy from sw_stats
/*

   enum overlay_plots stat_selector;

   std::string time;
*/

/*
   currentLogData.gpu_load = gpu_info.load;
   currentLogData.gpu_temp = gpu_info.temp;
   currentLogData.gpu_core_clock = gpu_info.CoreClock;
   currentLogData.gpu_mem_clock = gpu_info.MemClock;
   currentLogData.gpu_vram_used = gpu_info.memoryUsed;
#ifdef __gnu_linux__
   currentLogData.ram_used = memused;
#endif


   currentLogData.cpu_load = cpuStats.GetCPUDataTotal().percent;
   currentLogData.cpu_temp = cpuStats.GetCPUDataTotal().temp;

*/


/*
struct fps_limit {
   Clock::time_point frameStart;
   Clock::time_point frameEnd;
   Clock::duration targetFrameTime;
   Clock::duration frameOverhead;
   Clock::duration sleepTime;
};
   
struct benchmark_stats {
   float total;
   std::vector<float> fps_data;
   std::vector<std::pair<std::string, float>> percentile_data;
};
   
struct LOAD_DATA {
   ImVec4 color_low;
   ImVec4 color_med;
   ImVec4 color_high;
   unsigned med_load;
   unsigned high_load;
};
   
extern struct fps_limit fps_limit_stats;
extern int32_t deviceID;

extern struct benchmark_stats benchmark;
extern ImVec2 real_font_size;
extern std::string wineVersion;
extern std::vector<logData> graph_data;


*/

   // TODO: FpsLimitter settings
   // TODO: HUD toggle
   // TODO: Logging (local and remote) toggle.
   // TODO: Config reload.
   // TODO: Use remote config.
   // TODO: Font types, sizes and colors.
   // TODO: HUD position (x, y) relative to selected 8-9 possible positions (i.e. top-left, top-right, ...).
   // TODO: Order of elements in HUD.

   return 0;
}

static int mango_message_handler(const Message* const message, void* my_state) {
   assert(message != NULL);
   assert(my_state != NULL);

   // DEBUG(fprintf(stderr, "client_request_handler called\n"));

   struct swapchain_stats *const sw_stats = (struct swapchain_stats *)my_state;

   return 0;
}


void update_hw_info(struct swapchain_stats& sw_stats, struct overlay_params& params, uint32_t vendorID)
{
   if (params.enabled[OVERLAY_PARAM_ENABLED_cpu_stats] || logger->is_active()) {
      cpuStats.UpdateCPUData();
#ifdef __gnu_linux__

      if (params.enabled[OVERLAY_PARAM_ENABLED_core_load] || params.enabled[OVERLAY_PARAM_ENABLED_cpu_mhz])
         cpuStats.UpdateCoreMhz();
      if (params.enabled[OVERLAY_PARAM_ENABLED_cpu_temp] || logger->is_active() || params.enabled[OVERLAY_PARAM_ENABLED_graphs])
         cpuStats.UpdateCpuTemp();
      if (params.enabled[OVERLAY_PARAM_ENABLED_cpu_power])
         cpuStats.UpdateCpuPower();
#endif
   }
   if (params.enabled[OVERLAY_PARAM_ENABLED_gpu_stats] || logger->is_active()) {
      if (vendorID == 0x1002)
         getAmdGpuInfo();

      if (vendorID == 0x10de)
         getNvidiaGpuInfo();
   }

   // get ram usage/max

#ifdef __gnu_linux__
   if (params.enabled[OVERLAY_PARAM_ENABLED_ram] || params.enabled[OVERLAY_PARAM_ENABLED_swap] || logger->is_active())
      update_meminfo();
   if (params.enabled[OVERLAY_PARAM_ENABLED_io_read] || params.enabled[OVERLAY_PARAM_ENABLED_io_write])
      getIoStats(&sw_stats.io);
#endif

   currentLogData.gpu_load = gpu_info.load;
   currentLogData.gpu_temp = gpu_info.temp;
   currentLogData.gpu_core_clock = gpu_info.CoreClock;
   currentLogData.gpu_mem_clock = gpu_info.MemClock;
   currentLogData.gpu_vram_used = gpu_info.memoryUsed;
#ifdef __gnu_linux__
   currentLogData.ram_used = memused;
#endif

   currentLogData.cpu_load = cpuStats.GetCPUDataTotal().percent;
   currentLogData.cpu_temp = cpuStats.GetCPUDataTotal().temp;
   // Save data for graphs
   if (graph_data.size() > 50)
      graph_data.erase(graph_data.begin());
#ifdef _WIN32
   float memused = 0;
#endif
   graph_data.push_back({0, 0, cpuStats.GetCPUDataTotal().percent, gpu_info.load, cpuStats.GetCPUDataTotal().temp,
                        gpu_info.temp, gpu_info.CoreClock, gpu_info.MemClock, gpu_info.memoryUsed, memused});
   logger->notify_data_valid();
   HUDElements.update_exec();
}

void update_hud_info(struct swapchain_stats& sw_stats, struct overlay_params& params, uint32_t vendorID){
   uint32_t f_idx = sw_stats.n_frames % ARRAY_SIZE(sw_stats.frames_stats);
   uint64_t now = os_time_get(); /* us */
   double elapsed = (double)(now - sw_stats.last_fps_update); /* us */
   fps = 1000000.0f * sw_stats.n_frames_since_update / elapsed;
   if (logger->is_active())
      benchmark.fps_data.push_back(fps);

   if (sw_stats.last_present_time) {
        sw_stats.frames_stats[f_idx].stats[OVERLAY_PLOTS_frame_timing] =
            now - sw_stats.last_present_time;
   }

   frametime = now - sw_stats.last_present_time;
   if (elapsed >= params.fps_sampling_period) {
      std::thread(update_hw_info, std::ref(sw_stats), std::ref(params), vendorID).detach();
      sw_stats.fps = fps;

      if (params.enabled[OVERLAY_PARAM_ENABLED_time]) {
         std::time_t t = std::time(nullptr);
         std::stringstream time;
         time << std::put_time(std::localtime(&t), params.time_format.c_str());
         sw_stats.time = time.str();
      }

      sw_stats.n_frames_since_update = 0;
      sw_stats.last_fps_update = now;

   }

   if (params.log_interval == 0){
      logger->try_log();
   }

   sw_stats.last_present_time = now;
   sw_stats.n_frames++;
   sw_stats.n_frames_since_update++;

   struct RpcClientState *const rpc_client_state = &(sw_stats.rpc_client_state);

   if (!sw_stats.rpc_client_state_initialized) {
       if (rpc_client_connect(rpc_client_state) == 0) {
          rpc_client_state->connected = 1;
       }
       // Even if we fail to connect. Still mark as initialized.
       sw_stats.rpc_client_state_initialized = 1;
   }

   rpc_client_maybe_communicate(rpc_client_state,
                                &mango_message_generator, (void*)(&sw_stats),
                                &mango_message_handler, (void*)(&sw_stats));
}

void calculate_benchmark_data(void *params_void){
   overlay_params *params = reinterpret_cast<overlay_params *>(params_void);

   vector<float> sorted = benchmark.fps_data;
   std::sort(sorted.begin(), sorted.end());
   benchmark.percentile_data.clear();

   benchmark.total = 0.f;
   for (auto fps_ : sorted){
      benchmark.total = benchmark.total + fps_;
   }

   size_t max_label_size = 0;

   for (std::string percentile : params->benchmark_percentiles) {
      float result;

      // special case handling for a mean-based average
      if (percentile == "AVG") {
         result = benchmark.total / sorted.size();
      } else {
         // the percentiles are already validated when they're parsed from the config.
         float fraction = parse_float(percentile) / 100;

         result = sorted[(fraction * sorted.size()) - 1];
         percentile += "%";
      }

      if (percentile.length() > max_label_size)
         max_label_size = percentile.length();

      benchmark.percentile_data.push_back({percentile, result});
   }

   for (auto& entry : benchmark.percentile_data) {
      entry.first.append(max_label_size - entry.first.length(), ' ');
   }
}


float get_time_stat(void *_data, int _idx)
{
   struct swapchain_stats *data = (struct swapchain_stats *) _data;
   if ((ARRAY_SIZE(data->frames_stats) - _idx) > data->n_frames)
      return 0.0f;
   int idx = ARRAY_SIZE(data->frames_stats) +
      data->n_frames < ARRAY_SIZE(data->frames_stats) ?
      _idx - data->n_frames :
      _idx + data->n_frames;
   idx %= ARRAY_SIZE(data->frames_stats);
   /* Time stats are in us. */
   return data->frames_stats[idx].stats[data->stat_selector] / data->time_dividor;
}

void position_layer(struct swapchain_stats& data, struct overlay_params& params, ImVec2 window_size)
{
   unsigned width = ImGui::GetIO().DisplaySize.x;
   unsigned height = ImGui::GetIO().DisplaySize.y;
   float margin = 10.0f;
   if (params.offset_x > 0 || params.offset_y > 0)
      margin = 0.0f;

   ImGui::SetNextWindowBgAlpha(params.background_alpha);
   ImGui::SetNextWindowSize(window_size, ImGuiCond_Always);
   ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
   ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8,-3));
   ImGui::PushStyleVar(ImGuiStyleVar_Alpha, params.alpha);
   switch (params.position) {
   case LAYER_POSITION_TOP_LEFT:
      data.main_window_pos = ImVec2(margin + params.offset_x, margin + params.offset_y);
      ImGui::SetNextWindowPos(data.main_window_pos, ImGuiCond_Always);
      break;
   case LAYER_POSITION_TOP_RIGHT:
      data.main_window_pos = ImVec2(width - window_size.x - margin + params.offset_x, margin + params.offset_y);
      ImGui::SetNextWindowPos(data.main_window_pos, ImGuiCond_Always);
      break;
   case LAYER_POSITION_BOTTOM_LEFT:
      data.main_window_pos = ImVec2(margin + params.offset_x, height - window_size.y - margin + params.offset_y);
      ImGui::SetNextWindowPos(data.main_window_pos, ImGuiCond_Always);
      break;
   case LAYER_POSITION_BOTTOM_RIGHT:
      data.main_window_pos = ImVec2(width - window_size.x - margin + params.offset_x, height - window_size.y - margin + params.offset_y);
      ImGui::SetNextWindowPos(data.main_window_pos, ImGuiCond_Always);
      break;
   case LAYER_POSITION_TOP_CENTER:
      data.main_window_pos = ImVec2((width / 2) - (window_size.x / 2), margin + params.offset_y);
      ImGui::SetNextWindowPos(data.main_window_pos, ImGuiCond_Always);
      break;
   }
}

void right_aligned_text(ImVec4& col, float off_x, const char *fmt, ...)
{
   ImVec2 pos = ImGui::GetCursorPos();
   char buffer[32] {};

   va_list args;
   va_start(args, fmt);
   vsnprintf(buffer, sizeof(buffer), fmt, args);
   va_end(args);

   ImVec2 sz = ImGui::CalcTextSize(buffer);
   ImGui::SetCursorPosX(pos.x + off_x - sz.x);
   //ImGui::Text("%s", buffer);
   ImGui::TextColored(col,"%s",buffer);
}

void center_text(std::string& text)
{
   ImGui::SetCursorPosX((ImGui::GetWindowSize().x / 2 )- (ImGui::CalcTextSize(text.c_str()).x / 2));
}

float get_ticker_limited_pos(float pos, float tw, float& left_limit, float& right_limit)
{
   //float cw = ImGui::GetContentRegionAvailWidth() * 3; // only table cell worth of width
   float cw = ImGui::GetWindowContentRegionMax().x - ImGui::GetStyle().WindowPadding.x;
   float new_pos_x = ImGui::GetCursorPosX();
   left_limit = cw - tw + new_pos_x;
   right_limit = new_pos_x;

   if (cw < tw) {
      new_pos_x += pos;
      // acts as a delay before it starts scrolling again
      if (new_pos_x < left_limit)
         return left_limit;
      else if (new_pos_x > right_limit)
         return right_limit;
      else
         return new_pos_x;
   }
   return new_pos_x;
}

#ifdef HAVE_DBUS
void render_mpris_metadata(struct overlay_params& params, mutexed_metadata& meta, uint64_t frame_timing, bool is_main)
{
   if (meta.meta.valid) {
      auto color = ImGui::ColorConvertU32ToFloat4(params.media_player_color);
      ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8,0));
      ImGui::Dummy(ImVec2(0.0f, 20.0f));
      //ImGui::PushFont(data.font1);

      if (meta.ticker.needs_recalc) {
         meta.ticker.tw0 = ImGui::CalcTextSize(meta.meta.title.c_str()).x;
         meta.ticker.tw1 = ImGui::CalcTextSize(meta.meta.artists.c_str()).x;
         meta.ticker.tw2 = ImGui::CalcTextSize(meta.meta.album.c_str()).x;
         meta.ticker.longest = std::max(std::max(
               meta.ticker.tw0,
               meta.ticker.tw1),
            meta.ticker.tw2);
         meta.ticker.needs_recalc = false;
      }

      float new_pos, left_limit = 0, right_limit = 0;
      get_ticker_limited_pos(meta.ticker.pos, meta.ticker.longest, left_limit, right_limit);

      if (meta.ticker.pos < left_limit - g_overflow * .5f) {
         meta.ticker.dir = -1;
         meta.ticker.pos = (left_limit - g_overflow * .5f) + 1.f /* random */;
      } else if (meta.ticker.pos > right_limit + g_overflow) {
         meta.ticker.dir = 1;
         meta.ticker.pos = (right_limit + g_overflow) - 1.f /* random */;
      }

      meta.ticker.pos -= .5f * (frame_timing / 16666.7f) * meta.ticker.dir;

      for (auto order : params.media_player_order) {
         switch (order) {
            case MP_ORDER_TITLE:
            {
               new_pos = get_ticker_limited_pos(meta.ticker.pos, meta.ticker.tw0, left_limit, right_limit);
               ImGui::SetCursorPosX(new_pos);
               ImGui::TextColored(color, "%s", meta.meta.title.c_str());
            }
            break;
            case MP_ORDER_ARTIST:
            {
               new_pos = get_ticker_limited_pos(meta.ticker.pos, meta.ticker.tw1, left_limit, right_limit);
               ImGui::SetCursorPosX(new_pos);
               ImGui::TextColored(color, "%s", meta.meta.artists.c_str());
            }
            break;
            case MP_ORDER_ALBUM:
            {
               //ImGui::NewLine();
               if (!meta.meta.album.empty()) {
                  new_pos = get_ticker_limited_pos(meta.ticker.pos, meta.ticker.tw2, left_limit, right_limit);
                  ImGui::SetCursorPosX(new_pos);
                  ImGui::TextColored(color, "%s", meta.meta.album.c_str());
               }
            }
            break;
            default: break;
         }
      }

      if (!meta.meta.playing) {
         ImGui::TextColored(color, "(paused)");
      }

      //ImGui::PopFont();
      ImGui::PopStyleVar();
   }
}
#endif

void render_benchmark(swapchain_stats& data, struct overlay_params& params, ImVec2& window_size, unsigned height, Clock::time_point now){
   // TODO, FIX LOG_DURATION FOR BENCHMARK
   int benchHeight = (2 + benchmark.percentile_data.size()) * real_font_size.x + 10.0f + 58;
   ImGui::SetNextWindowSize(ImVec2(window_size.x, benchHeight), ImGuiCond_Always);
   if (height - (window_size.y + data.main_window_pos.y + 5) < benchHeight)
      ImGui::SetNextWindowPos(ImVec2(data.main_window_pos.x, data.main_window_pos.y - benchHeight - 5), ImGuiCond_Always);
   else
      ImGui::SetNextWindowPos(ImVec2(data.main_window_pos.x, data.main_window_pos.y + window_size.y + 5), ImGuiCond_Always);

   float display_time = std::chrono::duration<float>(now - logger->last_log_end()).count();
   static float display_for = 10.0f;
   float alpha;
   if(params.background_alpha != 0){
      if (display_for >= display_time){
         alpha = display_time * params.background_alpha;
         if (alpha >= params.background_alpha){
            ImGui::SetNextWindowBgAlpha(params.background_alpha);
         }else{
            ImGui::SetNextWindowBgAlpha(alpha);
         }
      } else {
         alpha = 6.0 - display_time * params.background_alpha;
         if (alpha >= params.background_alpha){
            ImGui::SetNextWindowBgAlpha(params.background_alpha);
         }else{
            ImGui::SetNextWindowBgAlpha(alpha);
         }
      }
   } else {
      if (display_for >= display_time){
         alpha = display_time * 0.0001;
         ImGui::SetNextWindowBgAlpha(params.background_alpha);
      } else {
         alpha = 6.0 - display_time * 0.0001;
         ImGui::SetNextWindowBgAlpha(params.background_alpha);
      }
   }
   ImGui::Begin("Benchmark", &open, ImGuiWindowFlags_NoDecoration);
   static const char* finished = "Logging Finished";
   ImGui::SetCursorPosX((ImGui::GetWindowSize().x / 2 )- (ImGui::CalcTextSize(finished).x / 2));
   ImGui::TextColored(ImVec4(1.0, 1.0, 1.0, alpha / params.background_alpha), "%s", finished);
   ImGui::Dummy(ImVec2(0.0f, 8.0f));
   char duration[20];
   snprintf(duration, sizeof(duration), "Duration: %.1fs", std::chrono::duration<float>(logger->last_log_end() - logger->last_log_begin()).count());
   ImGui::SetCursorPosX((ImGui::GetWindowSize().x / 2 )- (ImGui::CalcTextSize(duration).x / 2));
   ImGui::TextColored(ImVec4(1.0, 1.0, 1.0, alpha / params.background_alpha), "%s", duration);
   for (auto& data_ : benchmark.percentile_data){
      char buffer[20];
      snprintf(buffer, sizeof(buffer), "%s %.1f", data_.first.c_str(), data_.second);
      ImGui::SetCursorPosX((ImGui::GetWindowSize().x / 2 )- (ImGui::CalcTextSize(buffer).x / 2));
      ImGui::TextColored(ImVec4(1.0, 1.0, 1.0, alpha / params.background_alpha), "%s %.1f", data_.first.c_str(), data_.second);
   }
   float max = *max_element(benchmark.fps_data.begin(), benchmark.fps_data.end());
   ImVec4 plotColor = HUDElements.colors.frametime;
   plotColor.w = alpha / params.background_alpha;
   ImGui::PushStyleColor(ImGuiCol_PlotLines, plotColor);
   ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.0, 0.0, 0.0, alpha / params.background_alpha));
   ImGui::Dummy(ImVec2(0.0f, 8.0f));
   if (params.enabled[OVERLAY_PARAM_ENABLED_histogram])
      ImGui::PlotHistogram("", benchmark.fps_data.data(), benchmark.fps_data.size(), 0, "", 0.0f, max + 10, ImVec2(ImGui::GetContentRegionAvailWidth(), 50));
   else
      ImGui::PlotLines("", benchmark.fps_data.data(), benchmark.fps_data.size(), 0, "", 0.0f, max + 10, ImVec2(ImGui::GetContentRegionAvailWidth(), 50));
   ImGui::PopStyleColor(2);
   ImGui::End();
}

ImVec4 change_on_load_temp(LOAD_DATA& data, unsigned current)
{
   if (current >= data.high_load){
      return data.color_high;
      }
   else if (current >= data.med_load){
      float diff = float(current - data.med_load) / float(data.high_load - data.med_load);
      float x = (data.color_high.x - data.color_med.x) * diff;
      float y = (data.color_high.y - data.color_med.y) * diff;
      float z = (data.color_high.z - data.color_med.z) * diff;
      return ImVec4(data.color_med.x + x, data.color_med.y + y, data.color_med.z + z, 1.0);
   } else {
      float diff = float(current) / float(data.med_load);
      float x = (data.color_med.x - data.color_low.x) * diff;
      float y = (data.color_med.y - data.color_low.y) * diff;
      float z = (data.color_med.z - data.color_low.z) * diff;
      return ImVec4(data.color_low.x + x, data.color_low.y + y, data.color_low.z + z, 1.0);
   }
}

void render_imgui(swapchain_stats& data, struct overlay_params& params, ImVec2& window_size, bool is_vulkan)
{
   HUDElements.sw_stats = &data; HUDElements.params = &params;
   HUDElements.is_vulkan = is_vulkan;
   ImGui::GetIO().FontGlobalScale = params.font_scale;
   if(!logger) logger = std::make_unique<Logger>(&params);
   static float ralign_width = 0, old_scale = 0;
   window_size = ImVec2(params.width, params.height);
   unsigned height = ImGui::GetIO().DisplaySize.y;
   auto now = Clock::now();

   if (old_scale != params.font_scale) {
      HUDElements.ralign_width = ralign_width = ImGui::CalcTextSize("A").x * 4 /* characters */;
      old_scale = params.font_scale;
   }

   if (!params.no_display){
      ImGui::Begin("Main", &open, ImGuiWindowFlags_NoDecoration);
      ImGui::BeginTable("hud", params.table_columns, ImGuiTableFlags_NoClip);
      HUDElements.place = 0;
      for (auto& func : HUDElements.ordered_functions){
         func.first();
         HUDElements.place += 1;
      }
      ImGui::EndTable();

      if(logger->is_active())
         ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(data.main_window_pos.x + window_size.x - 15, data.main_window_pos.y + 15), 10, params.engine_color, 20);
      window_size = ImVec2(window_size.x, ImGui::GetCursorPosY() + 10.0f);
      ImGui::End();
      if((now - logger->last_log_end()) < 12s)
         render_benchmark(data, params, window_size, height, now);
   }
}
