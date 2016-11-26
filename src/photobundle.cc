#include <ceres/ceres.h>
#include <ceres/rotation.h>

#include "types.h"
#include "photobundle.h"
#include "sample_eigen.h"

#include <cmath>
#include <type_traits>
#include <iterator>
#include <algorithm>
#include <map>

// this is just for YCM to stop highlighting openmp as error (there is no openmp
// in clang3.5)
#define HAS_OPENMP __GNUC__ >= 4 && __clang__ == 0

#if HAS_OPENMP
#include <omp.h>
#endif


/**
 * \return bilinearly interpolated pixel value at subpixel location (xf,yf)
 */
template <class Image, class T> inline
T interp2(const Image& I, T xf, T yf, T fillval = 0.0, T offset = 0.0)
{
  const int max_cols = I.cols() - 1;
  const int max_rows = I.rows() - 1;

  xf += offset;
  yf += offset;

  int xi = (int) std::floor(xf);
  int yi = (int) std::floor(yf);

  xf -= xi;
  yf -= yi;

  if( xi >= 0 && xi < max_cols && yi >= 0 && yi < max_rows )
  {
    const T wx = 1.0 - xf;
    return (1.0 - yf) * ( I(yi,   xi)*wx + I(yi,   xi+1)*xf )
               +  yf  * ( I(yi+1, xi)*wx + I(yi+1, xi+1)*xf );
  } else
  {
    if( xi == max_cols && yi < max_rows )
      return ( xf > 0 ) ? fillval : (1.0-yf)*I(yi,xi) + yf*I(yi+1, xi);
    else if( yi == max_rows && xi < max_cols )
      return ( yf > 0 ) ? fillval : (1.0-xf)*I(yi,xi) + xf*I(yi, xi+1);
    else if( xi == max_cols && yi == max_rows )
      return ( xf > 0 || yf > 0 ) ? fillval : I(yi, xi);
    else
      return fillval;
  }

}

template <int N> constexpr int square() { return N*N; }

template <int R, class ImageType, class ProjType, typename T = double>
void interpolateFixedPatch(Vec_<T, square<2*R+1>()>& dst,
                           const ImageType& I, const ProjType& p,
                           const T& fillval = T(0), const T& offset = T(0))
{
  const T x = static_cast<T>( p[0] + offset );
  const T y = static_cast<T>( p[1] + offset );

  for(int c = -R, i=0; c <= R; ++c) { // NOTE: this does col-major
    const T xf = x + static_cast<T>(c);
    for(int r = -R; r <= R; ++r, ++i) {
      const T yf = y + static_cast<T>(r);
      dst[i] = interp2(I, xf, yf, fillval);
    }
  }
}


template <int R, class ImageType, class ProjType, typename T = double>
void copyFixedPatch(Vec_<T, square<2*R+1>()>& dst, const ImageType& I,
                    const ProjType& p)
{
  const int x = static_cast<int>( std::round( p[0] ) );
  const int y = static_cast<int>( std::round( p[1] ) );

  int max_cols = I.cols() - 1;
  int max_rows = I.rows() - 1;

  for(int c = -R, i=0; c <= R; ++c) {
    int xf = std::max(R, std::min(x + c, max_cols));
    for(int r = -R; r <= R; ++r, ++i) {
      int yf = std::max(R, std::min(y + r, max_rows));
      dst[i] = I(yf, xf);
    }
  }
}

template <int R, typename T = float>
class ZnccPatch
{
  static_assert(std::is_floating_point<T>::value, "T must be floating point");

 public:
  static constexpr int Radius = R;
  static constexpr int Dimension = (2*R+1) * (2*R+1);

 public:
  inline ZnccPatch() {}

  template <class ImageType, class ProjType> inline
  ZnccPatch(const ImageType& image, const ProjType& uv) { set(image, uv); }

  template <class ImageType, class ProjType> inline
  const ZnccPatch& set(const ImageType& I, const ProjType& uv)
  {
    interpolateFixedPatch<R>(_data, I, uv, T(0.0), T(0.0));
    T mean = _data.array().sum() / (T) _data.size();
    _data.array() -= mean;
    _norm = _data.norm();

    return *this;
  }

  template <class ImageType, class ProjType>
  inline static ZnccPatch FromImage(const ImageType& I, const ProjType& p)
  {
    ZnccPatch ret;
    ret.set(I, p);
    return ret;
  }

  inline T score(const ZnccPatch& other) const
  {
    T d = _norm * other._norm;
    return d > 1e-6 ? _data.dot(other._data) / d : -1.0;
  }

 private:
  Vec_<T, Dimension> _data;
  T _norm;

 public:
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW;
}; // ZnccPatch


/**
 */
struct PhotometricBundleAdjustment::ScenePoint
{
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW;

  typedef std::vector<uint32_t>        VisibilityList;
  typedef EigenAlignedContainer_<Vec2> ProjectionList;
  typedef ZnccPatch<2, float>          ZnccPatchType;

  /**
   * Create a scene point with position 'X' and reference frame number 'f_id'
   *
   * We also store the original point for later comparision
   */
  inline ScenePoint(const Vec3& X, uint32_t f_id)
      : _X(X), _X_original(X)
  {
    _f.reserve(8);
    _f.push_back(f_id);
  }

  /**
   * \return true if the scene point has 'f_id' it is visibility list
   */
  inline bool hasFrame(uint32_t f_id) const {
    return std::find(_f.begin(), _f.end(), f_id) != _f.end();
  }

  /**
   * \return the visibility list
   */
  inline const VisibilityList& visibilityList() const { return _f; }

  /** \return the reference frame number (also the first in the list) */
  inline const uint32_t& refFrameId() const { return _f.front(); }

  /** \return the last frame number, most recent observation */
  inline const uint32_t& lastFrameId() const { return _f.back(); }

  /** \return the 3D point associated with the ScenePoint */
  inline const Vec3& X() const { return _X; }
  inline       Vec3& X()       { return _X; }

  /** \return the original 3D point */
  inline const Vec3& getOriginalPoint() const { return _X_original; }

  /** \return the associated patch */
  inline const ZnccPatchType& patch() const { return _patch; }

  inline void addFrame(uint32_t f) { _f.push_back(f); }

  template <class ImageType, class ProjType> inline
  void setZnccPach(const ImageType& I, const ProjType& x)
  {
    _patch.set(I, x);
  }

  inline const std::vector<double>& descriptor() const { return _descriptor; }
  inline       std::vector<double>& descriptor()       { return _descriptor; }

  inline void setSaliency(double v) { _saliency = v; }
  inline const double& getSaliency() const { return _saliency; }

  inline void setRefined(bool v) { _was_refined = v; }
  inline const bool& wasRefined() const { return _was_refined; }

  inline size_t numFrames() const { return _f.size(); }

  inline void setFirstProjection(const Vec_<int,2>& x) { _x = x; }
  inline const Vec_<int,2>& getFirstProjection() const { return _x; }

  Vec3 _X;
  Vec3 _X_original;
  VisibilityList _f;
  ZnccPatchType _patch;
  std::vector<double> _descriptor;

  double _saliency  = 0.0;
  bool _was_refined = false;

  Vec_<int,2> _x;
}; // ScenePoint

struct PhotometricBundleAdjustment::DescriptorFrame
{
}; // DescriptorFrame


PhotometricBundleAdjustment::PhotometricBundleAdjustment(
    const Calibration& calib, const ImageSize& image_size, const Options& options)
  : _calib(calib), _image_size(image_size), _options(options) {}


PhotometricBundleAdjustment::~PhotometricBundleAdjustment() {}

void PhotometricBundleAdjustment::
addFrame(const uint8_t* I_ptr, const float* Z_ptr, const Mat44& T, Result* result)
{
}