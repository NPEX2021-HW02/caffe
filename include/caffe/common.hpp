#ifndef CAFFE_COMMON_HPP_
#define CAFFE_COMMON_HPP_

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <boost/version.hpp>

#include <boost/thread.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>
#include <boost/array.hpp>

#include <atomic>
#include <condition_variable>
#include <climits>
#include <cmath>
#include <fstream>  // NOLINT(readability/streams)
#include <iostream>  // NOLINT(readability/streams)
#include <map>
#include <unordered_map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>  // pair
#include <vector>
#ifdef USE_CUDNN
#  include <cudnn.h>
#endif

#include "caffe/util/device_alternate.hpp"
#include "caffe/util/float16.hpp"
#include "caffe/util/gpu_memory.hpp"
#include "caffe/macros.hpp"

// A simple macro to mark codes that are not implemented, so that when the code
// // is executed we will see a fatal log.
#define NOT_IMPLEMENTED LOG(FATAL) << "Not Implemented Yet"

// See PR #1236
namespace cv { class Mat; }

namespace caffe {

std::uint32_t lwp_id();
std::uint64_t lwp_dev_id(int dev = -1);

template<typename Dtype>
void atomic_maximum(std::atomic<Dtype>& max_val, Dtype const& new_val) noexcept {
  Dtype prev_val = std::atomic_load(&max_val);
  while (prev_val < new_val &&
         !max_val.compare_exchange_weak(prev_val, new_val)) {}
}

template<typename Dtype>
void atomic_minimum(std::atomic<Dtype>& min_val, Dtype const& new_val) noexcept {
  Dtype prev_val = std::atomic_load(&min_val);
  while (prev_val > new_val &&
         !min_val.compare_exchange_weak(prev_val, new_val)) {}
}

// Shared CUDA Stream for correct life cycle management
class CudaStream {
  explicit CudaStream(bool high_priority);

 public:
  ~CudaStream();

  static shared_ptr<CudaStream> create(bool high_priority = false) {
    shared_ptr<CudaStream> pstream(new CudaStream(high_priority));
    return pstream;
  }

  cudaStream_t get() const {
    return stream_;
  }

 private:
  cudaStream_t stream_;
  DISABLE_COPY_MOVE_AND_ASSIGN(CudaStream);
};

struct CuBLASHandle {
  CuBLASHandle();
  explicit CuBLASHandle(shared_ptr<CudaStream> stream);
  ~CuBLASHandle();

  cublasHandle_t get() const {
    return handle_;
  }
 private:
  cublasHandle_t handle_;
  shared_ptr<CudaStream> stream_;
  DISABLE_COPY_MOVE_AND_ASSIGN(CuBLASHandle);
};

#ifdef USE_CUDNN
struct CuDNNHandle {
  explicit CuDNNHandle(shared_ptr<CudaStream> stream);
  ~CuDNNHandle();

  cudnnHandle_t get() const {
    return handle_;
  }
 private:
  cudnnHandle_t handle_;
  shared_ptr<CudaStream> stream_;
  DISABLE_COPY_MOVE_AND_ASSIGN(CuDNNHandle);
};
#endif

// A global initialization function that you should call in your main function.
// Currently it initializes google flags and google logging.
void GlobalInit(int* pargc, char*** pargv);

// A singleton class to hold common caffe stuff, such as the handler that
// caffe is going to use for cublas, curand, etc.
class Caffe {
 public:
  ~Caffe();

  // Thread local context for Caffe. Moved to common.cpp instead of
  // including boost/thread.hpp to avoid a boost/NVCC issues (#1009, #1010)
  // on OSX. Also fails on Linux with CUDA 7.0.18.
  static Caffe& Get();

  enum Brew { CPU, GPU };

  // This random number generator facade hides boost and CUDA rng
  // implementation from one another (for cross-platform compatibility).
  class RNG {
   public:
    RNG();
    explicit RNG(uint64_t seed);
    RNG(const RNG&);
    RNG& operator = (const RNG&);
    void* generator();
   private:
    class Generator;
    shared_ptr<Generator> generator_;
  };

  // Getters for boost rng, curand, and cublas handles
  static RNG& rng_stream() {
    Caffe& c = Get();
    if (!c.random_generator_) {
      c.random_generator_.reset(new RNG());
    }
    return *(c.random_generator_);
  }
  static cudaStream_t thread_stream(int group = 0) {
    return Get().pstream(group)->get();
  }
  static cublasHandle_t cublas_handle(int group) {
    return Get().th_cublas_handle(group)->get();
  }
  static curandGenerator_t curand_generator() {
    return Get().curand_generator_;
  }
  static cudaStream_t curand_stream() {
    return Get().curand_stream_->get();
  }
  static shared_ptr<CudaStream> thread_pstream(int group = 0) {
    return Get().pstream(group);
  }
#ifdef USE_CUDNN
  static cudnnHandle_t cudnn_handle(int group) {
    return Get().th_cudnn_handle(group);
  }
#endif
  static caffe::GPUMemory::Workspace& ws(size_t id) {
    return Get()._ws(id);
  }
  static int device() {
    return Get()._device();
  }

  static void report_epoch_count(size_t rec) {
    atomic_minimum(epoch_count_, rec);
  }
  static size_t epoch_count() {
    size_t count = epoch_count_.load();
    if (count == (size_t)-1L) {
      count = 0UL;
    }
    return count;
  }

  // Returns the mode: running on CPU or GPU.
  static Brew mode() {
    return mode_;
  }
  // The setters for the variables
  // Sets the mode. It is recommended that you don't change the mode halfway
  // into the program since that may cause allocation of pinned memory being
  // freed in a non-pinned way, which may cause problems - I haven't verified
  // it personally but better to note it here in the header file.
  static void set_mode(Brew mode) {
    if (mode_ == mode) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(caffe_mutex_);
      DLOG(INFO) << "Caffe " << " old mode "
                 << (mode_ == Caffe::GPU ? "GPU" : "CPU") << " new mode "
                 << (mode == Caffe::GPU ? "GPU" : "CPU");
      mode_ = mode;
    }
    Get().init();
  }
  // Next seed. It's deterministic if root seed is already set.
  static uint64_t next_seed();
  // Sets the random seed of both boost and curand
  // Uses system generated one if -1 passed
  static void set_random_seed(uint64_t random_seed = SEED_NOT_SET) {
    Get().set_random_seed_int(random_seed);
  }
  // For correct determinism user should set a seed for a root solver
  // Note: it invokes set_random_seed internally
  static void set_root_seed(uint64_t random_seed);
  // Sets the root device. Function name remains the same for backward compatibility.
  static void SetDevice(const int device_id);
  static int root_device() {
    return root_device_;
  }
  // Prints the current GPU status.
  static std::string DeviceQuery();
  // Check if specified device is available
  static bool CheckDevice(const int device_id);
  // Search from start_id to the highest possible device ordinal,
  // return the ordinal of the first available device.
  static int FindDevice(const int start_id = 0);
  /// All physical devices regardless of usage
  static int device_count();
  // Parallel training info
  static size_t solver_count() {
    return solver_count_;
  }
  // Number of physical devices being used
  static int device_in_use_per_host_count() {
    return (int)gpus_.size();
  }
  static void set_solver_count(size_t val) {
    if (solver_count_ != val) {
      std::lock_guard<std::mutex> lock(caffe_mutex_);
      solver_count_ = val;
    }
  }
  static bool root_solver() { return Get().is_root_solver_; }
  static void set_root_solver(bool val) { Get().is_root_solver_ = val; }
  static int restored_iter() { return restored_iter_; }
  static void set_restored_iter(int val);

  static void set_gpus(const std::vector<int>& gpus) {
    std::lock_guard<std::mutex> lock(caffe_mutex_);
    gpus_ = gpus;
    if (gpus_.empty()) {
      gpus_.push_back(root_device_);
    }
  }
  static const std::vector<int>& gpus() {
    return gpus_;
  }
  static std::string time_from_init();
  static int device_capability(int device) {
    return props().device_capability(device);
  }
  static int current_device() {
    std::lock_guard<std::mutex> lock(cd_mutex_);
    int device = 0;
    cudaGetDevice(&device);
    return device;
  }

  /**
   * Minimum memory available across all deviced currently used
   * @return size_t
   */
  static size_t min_avail_device_memory();

  static int thread_count() {
    return thread_count_;
  }

  static constexpr uint64_t SEED_NOT_SET = static_cast<uint64_t>(-1);
  static constexpr int MAX_CONV_GROUPS = 2;
  static constexpr int GPU_TRANSF_GROUP = 2;

 protected:
  vector<shared_ptr<CudaStream>> streams_;
  shared_ptr<CudaStream> pstream(int group = 0);
  vector<shared_ptr<CuBLASHandle>> cublas_handles_;
  shared_ptr<CuBLASHandle> th_cublas_handle(int group = 0);
  curandGenerator_t curand_generator_;
#ifdef USE_CUDNN
  vector<shared_ptr<CuDNNHandle>> cudnn_handles_;
  cudnnHandle_t th_cudnn_handle(int group = 0);
#endif

  shared_ptr<RNG> random_generator_;
  bool is_root_solver_;
  const int device_;  // CUDA device where constructor was invoked

  // Default device chosen by a user and associated with the main thread.
  // For example, if user runs `caffe train -gpu=1,0,3` then it has to be set to 1.
  static int root_device_;
  static Brew mode_;
  static size_t solver_count_;
  static std::vector<int> gpus_;
  static int thread_count_;
  static int restored_iter_;
  static std::atomic<uint64_t> root_seed_;
  static std::mutex cd_mutex_, caffe_mutex_, pstream_mutex_, cublas_mutex_,
      cudnn_mutex_, seed_mutex_;

  static std::atomic<size_t> epoch_count_;
  shared_ptr<CudaStream> curand_stream_;

  // Workspaces used by all Convolution layers one after another.
  // We carry them global to prevent unnecessary allocations/deallocations
  // because they hurt performance. It's also shared between TRAIN and TESTS nets.
  caffe::GPUMemory::Workspace ws_[CAFFE_WS_TOTAL];

 private:
  // The private constructor to avoid duplicate instantiation.
  Caffe();

  void init();  // when Brew mode changes
  void set_random_seed_int(uint64_t random_seed);
  int _device() const {
    return device_;
  }
  caffe::GPUMemory::Workspace& _ws(size_t id) {
    return ws_[id];
  }
  const GPUMemory::Scope gpu_memory_scope_;

  DISABLE_COPY_MOVE_AND_ASSIGN(Caffe);

 public:
  // Caffe Properties singleton
  class Properties {
    friend class Caffe;

    Properties();

   public:
    const std::string& caffe_version() const {
      return caffe_version_;
    }
    const std::string& cudnn_version() const {
      return cudnn_version_;
    }
    const std::string& cublas_version() const {
      return cublas_version_;
    }
    const std::string& cuda_version() const {
      return cuda_version_;
    }
    const std::string& cuda_driver_version() const {
      return cuda_driver_version_;
    }
    std::string start_time() const {
      // NOLINT_NEXT_LINE(runtime/threadsafe_fn)
      return std::ctime(&init_time_);
    }
    std::time_t init_time() const {
      return init_time_;
    }
    int device_capability(int device) const {
      return compute_capabilities_[device];
    }

   private:
    std::time_t init_time_;
    std::string caffe_version_;
    std::string cudnn_version_;
    std::string cublas_version_;
    std::string cuda_version_;
    std::string cuda_driver_version_;
    std::vector<int> compute_capabilities_;

    DISABLE_COPY_MOVE_AND_ASSIGN(Properties);
  };

  static Properties& props();
};

// Yet another Event implementation
class Flag {
  mutable std::mutex m_;
  mutable std::condition_variable cv_;
  bool flag_, disarmed_;

  DISABLE_COPY_MOVE_AND_ASSIGN(Flag);

 public:
  explicit Flag(bool state = false)
      : flag_(state), disarmed_(false) {}

  bool is_set() const {
    std::lock_guard<std::mutex> lock(m_);
    return flag_;
  }

  void set() {
    {
      std::lock_guard<std::mutex> lock(m_);
      flag_ = true;
    }
    cv_.notify_all();
  }

  void reset() {
    {
      std::lock_guard<std::mutex> lock(m_);
      flag_ = false;
    }
    cv_.notify_all();
  }

  void wait() const {
    std::unique_lock<std::mutex> lock(m_);
    cv_.wait(lock, [this] { return flag_; } );
  }

  void disarm() {
    {
      std::lock_guard<std::mutex> lock(m_);
      disarmed_ = true;
    }
    cv_.notify_all();
  }

  void wait_reset() {
    {
      std::unique_lock<std::mutex> lock(m_);
      cv_.wait(lock, [this] { return flag_ || disarmed_; } );
      if (!disarmed_) {
        flag_ = false;
      }
    }
    cv_.notify_all();
  }
};

template <typename M>
class ThreadSafeMap {
 public:
  explicit ThreadSafeMap(std::mutex& m) : m_(m) {
    std::lock_guard<std::mutex> lock(m_);
    map_.reset(new M());
  }
  ~ThreadSafeMap() = default;

  using iterator = typename M::iterator;
  using const_iterator = typename M::const_iterator;
  using value_type = typename M::value_type;
  using mapped_type = typename M::mapped_type;
  using key_type = typename M::key_type;
  using size_type = typename M::size_type;

  size_type size() const {
    std::lock_guard<std::mutex> lock(m_);
    return map_->size();
  }
  std::pair<iterator, bool> insert(const value_type& entry) {
    std::lock_guard<std::mutex> lock(m_);
    return map_->insert(entry);
  }
  template<class... Args>
  std::pair<iterator, bool> emplace(Args&&... args) {
    std::lock_guard<std::mutex> lock(m_);
    return map_->emplace(args...);
  }
  mapped_type& operator[](const key_type& key) {
    std::lock_guard<std::mutex> lock(m_);
    return (*map_)[key];
  }
  iterator find(const key_type& key) {
    std::lock_guard<std::mutex> lock(m_);
    return map_->find(key);
  }
  const_iterator find(const key_type& key) const {
    std::lock_guard<std::mutex> lock(m_);
    return map_->find(key);
  }
  iterator begin() {
    std::lock_guard<std::mutex> lock(m_);
    return map_->begin();
  }
  const_iterator begin() const {
    std::lock_guard<std::mutex> lock(m_);
    return map_->begin();
  }
  const_iterator cbegin() const {
    std::lock_guard<std::mutex> lock(m_);
    return map_->cbegin();
  }
  iterator end() {
    std::lock_guard<std::mutex> lock(m_);
    return map_->end();
  }
  const_iterator end() const {
    std::lock_guard<std::mutex> lock(m_);
    return map_->end();
  }
  const_iterator cend() const {
    std::lock_guard<std::mutex> lock(m_);
    return map_->cend();
  }
  void clear() {
    std::lock_guard<std::mutex> lock(m_);
    map_->clear();
  }
  void insert_max(const key_type& key, const mapped_type& value) {
    std::lock_guard<std::mutex> lock(m_);
    iterator it = map_->find(key);
    if (it == map_->end()) {
      map_->emplace(key, value);
    } else if (value > it->second) {
      it->second = value;
    }
  }
  bool remove_top(key_type& key, mapped_type& value) {
    std::lock_guard<std::mutex> lock(m_);
    iterator it = map_->begin();
    if (it == map_->end()) {
      return false;
    }
    key = it->first;
    value = it->second;
    map_->erase(it);
    return true;
  }
  size_type erase(const key_type& key) {
    std::lock_guard<std::mutex> lock(m_);
    return map_->erase(key);
  }
  iterator erase(const_iterator pos) {
    std::lock_guard<std::mutex> lock(m_);
    return map_->erase(pos);
  }

 private:
  std::unique_ptr<M> map_;
  std::mutex& m_;
};


///> the biggest number n which is not greater than val and divisible by 2^power
template<int power, typename T>
inline T align_down(T val) {
  return val & ~((1 << power) - 1);
}

///> the smallest number n which is not less than val and divisible by 2^power
template<int power, typename T>
inline T align_up(T val) {
  return !(val & ((1 << power) - 1)) ? val : (val | ((1 << power) - 1)) + 1;
}

template <typename T>
inline bool is_even(T val) { return (val & 1) == 0; }

///> the smallest even number n which is not less than val
template <typename T>
inline T even(T val) { return val & 1 ? val + 1 : val; }


// Unlike dataType<> this one keeps typed values to be used in caffe_* calls.
template <typename Dtype> class TypedConsts;
template<> class TypedConsts<double>  {
 public:
  static const double zero, one;
};
template<> class TypedConsts<float>  {
 public:
  static const float zero, one;
};
template<> class TypedConsts<float16>  {
 public:
  static const float16 zero, one;
};
template<> class TypedConsts<int>  {
 public:
  static const int zero, one;
};


template <typename Dtype>
CAFFE_UTIL_IHD Dtype max_dtype();
template <>
CAFFE_UTIL_IHD double max_dtype<double>() {
  return DBL_MAX;
}
template <>
CAFFE_UTIL_IHD float max_dtype<float>() {
  return FLT_MAX;
}
template <>
CAFFE_UTIL_IHD float16 max_dtype<float16>() {
  float16 ret;
  // Exponent all ones except LSB (0x1e), mantissa is all ones (0x3ff)
  ret.setx(0x7bffU);
  return ret;
}
// Largest positive FP16 value, corresponds to 6.5504e+04
#ifdef __CUDACC__
template <>
CAFFE_UTIL_IHD half max_dtype<half>() {
    half ret;
    // Exponent all ones except LSB (0x1e), mantissa is all ones (0x3ff)
    ret.setx(0x7bffU);
    return ret;
}
#endif

// Normalized minimums:
template <typename Dtype>
CAFFE_UTIL_IHD Dtype min_dtype();
template <>
CAFFE_UTIL_IHD double min_dtype<double>() {
  return DBL_MIN;
}
template <>
CAFFE_UTIL_IHD float min_dtype<float>() {
  return FLT_MIN;
}
template <>
CAFFE_UTIL_IHD float16 min_dtype<float16>() {
  float16 ret;
  // Exponent is 0x01 (5 bits), mantissa is all zeros (10 bits)
  ret.setx(0x0400U);
  return ret;
}
// Smallest positive (normalized) FP16 value, corresponds to 6.1035e-05
#ifdef __CUDACC__
template <>
CAFFE_UTIL_IHD half min_dtype<half>() {
  half ret;
  // Exponent is 0x01 (5 bits), mantissa is all zeros (10 bits)
  ret.setx(0x0400U);
  return ret;
}
#endif

template <typename Dtype>
CAFFE_UTIL_IHD Dtype epsilon_dtype();
template <>
CAFFE_UTIL_IHD double epsilon_dtype<double>() {
  return DBL_EPSILON;
}
template <>
CAFFE_UTIL_IHD float epsilon_dtype<float>() {
  return FLT_EPSILON;
}
template <>
CAFFE_UTIL_IHD float16 epsilon_dtype<float16>() {
  float16 ret;
  ret.setx(0x1001U);
  return ret;
}
#ifdef __CUDACC__
template <>
CAFFE_UTIL_IHD half epsilon_dtype<half>() {
  half ret;
  ret.setx(0x1001U);
  return ret;
}
#endif

template <typename Dtype> constexpr
inline bool is_precise() {
  return false;
}
template <> constexpr
inline bool is_precise<double>() {
  return true;
}
template <> constexpr
inline bool is_precise<float>() {
  return true;
}

template <typename Dtype>
CAFFE_UTIL_IHD Dtype tol(Dtype fine, Dtype coarse) {
  return is_precise<Dtype>() ? fine : coarse;
}

template <typename Dtype>
CAFFE_UTIL_IHD Dtype tol2(Dtype fine, Dtype coarse, Dtype cpu_tol) {
  return Caffe::mode() == Caffe::CPU ? cpu_tol : (is_precise<Dtype>() ? fine : coarse);
}

template <typename Dtype>
std::string mem_fmt(Dtype val) {
  ostringstream os;
  if (val >= 1.e7) {
    os << std::round(val * 1.e-7) * 0.01F << "G";
  } else if (val >= 1.e4) {
    os << std::round(val * 1.e-4) * 0.01F << "M";
  } else if (val >= 1.e1) {
    os << std::round(val * 1.e-1) * 0.01F << "K";
  } else {
    os << val;
  }
  return os.str();
}

template <typename Dtype>
float f_round1(Dtype val) {
  return std::round(val * 10.F) * 0.1F;
}

template <typename Dtype>
float f_round2(Dtype val) {
  return std::round(val * 100.F) * 0.01F;
}

}  // namespace caffe

inline size_t rss() {
  size_t i = 0UL;
  FILE* file = fopen("/proc/self/status", "r");
  char line[128];
  while (fgets(line, sizeof(line), file) != nullptr) {
    if (strncmp(line, "VmRSS:", 6) == 0) {
      i = strlen(line);
      const char* p = line;
      while (*p <'0' || *p > '9') p++;
      line[i-3] = '\0';
      i = (size_t)atol(p);
      break;
    }
  }
  fclose(file);
  return i;
}

#endif  // CAFFE_COMMON_HPP_
