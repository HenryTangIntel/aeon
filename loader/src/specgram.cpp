/*
 Copyright 2016 Nervana Systems Inc.
 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.
*/

#include "specgram.hpp"

using cv::Mat;
using cv::Range;
using cv::Size;
using namespace std;
using std::stringstream;
using std::vector;

Specgram::Specgram(shared_ptr<const nervana::audio::config> params, int id)
: _feature(     params->_feature),
  _clipDuration(params->_clipDuration),
  _windowSize(  params->_windowSize),
  _stride(      params->_stride),
  _width(       params->_width),
  _numFreqs(    params->_windowSize / 2 + 1),
  _height(      params->_height),
  _samplingFreq(params->_samplingFreq),
  _numFilts(    params->_numFilts),
  _numCepstra(  params->_numCepstra),
  _window(      0),
  _rng(         id) {
    assert(_stride != 0);
    _maxSignalSize = params->_clipDuration * params->_samplingFreq / 1000;
    _bufSize = _width * _windowSize * MAX_BYTES_PER_SAMPLE;
    _buf = new char[_bufSize];
    if (params->_window != 0) {
        _window = new Mat(1, _windowSize, CV_32FC1);
        createWindow(params->_window);
    }
    assert(params->_randomScalePercent >= 0);
    assert(params->_randomScalePercent < 100);
    _scaleBy = params->_randomScalePercent / 100.0;
    _scaleMin = 1.0 - _scaleBy;
    _scaleMax = 1.0 + _scaleBy;
    transpose(getFilterbank(_numFilts, _windowSize, _samplingFreq), _fbank);

    assert(params->_width == (((params->_clipDuration * params->_samplingFreq / 1000) - params->_windowSize) / params->_stride) + 1);
}

Specgram::~Specgram() {
    delete _window;
    delete[] _buf;
}

int Specgram::generate(shared_ptr<RawMedia> raw, char* buf, int bufSize) {
    // TODO: get rid of these assumptions:
    cout << "channels: " << raw->channels() << endl;
    assert(raw->channels() == 1);
    assert(raw->bytesPerSample() == 2);

    assert(_width * _height == bufSize);
    int numWindows = stridedSignal(raw);
    assert(numWindows <= _width);
    Mat signal(numWindows, _windowSize, CV_16SC1, (short*) _buf);
    Mat input;
    signal.convertTo(input, CV_32FC1);

    applyWindow(input);
    Mat planes[] = {input, Mat::zeros(input.size(), CV_32FC1)};
    Mat compx;
    cv::merge(planes, 2, compx);

    cv::dft(compx, compx, cv::DFT_ROWS);
    compx = compx(Range::all(), Range(0, _numFreqs));

    cv::split(compx, planes);
    cv::magnitude(planes[0], planes[1], planes[0]);
    Mat mag;
    if (_feature == SPECGRAM) {
        mag = planes[0];
    } else {
        extractFeatures(planes[0], mag);
    }

    Mat feats;
    // Rotate by 90 degrees.
    cv::transpose(mag, feats);
    cv::flip(feats, feats, 0);

    cv::normalize(feats, feats, 0, 255, CV_MINMAX, CV_8UC1);
    Mat result(feats.rows, _width, CV_8UC1, buf);

    assert(feats.rows <= _height);

    feats.copyTo(result(Range(0, feats.rows), Range(0, feats.cols)));

    // Pad the rest with zeros.
    result(Range::all(), Range(feats.cols, result.cols)) = cv::Scalar::all(0);

    randomize(result);
    // Return the percentage of valid columns.
    return feats.cols * 100 / result.cols;
}

void Specgram::randomize(Mat& img) {
    if (_scaleBy > 0) {
        float fx = _rng.uniform(_scaleMin, _scaleMax);
        resize(img, fx);
    }
}

void Specgram::resize(Mat& img, float fx) {
    Mat dst;
    int inter = (fx > 1.0) ? CV_INTER_CUBIC : CV_INTER_AREA;
    cv::resize(img, dst, Size(), fx, 1.0, inter);
    assert(img.rows == dst.rows);
    if (img.cols > dst.cols) {
        dst.copyTo(img(Range::all(), Range(0, dst.cols)));
        img(Range::all(), Range(dst.cols, img.cols)) = cv::Scalar::all(0);
    } else {
        dst(Range::all(), Range(0, img.cols)).copyTo(img);
    }
}

void Specgram::none(int) {
}

void Specgram::hann(int steps) {
    for (int i = 0; i <= steps; i++) {
        _window->at<float>(0, i) = 0.5 - 0.5 * cos((2.0 * CV_PI * i) / steps);
    }
}

void Specgram::blackman(int steps) {
    for (int i = 0; i <= steps; i++) {
        _window->at<float>(0, i) = 0.42 -
                                   0.5 * cos((2.0 * CV_PI * i) / steps) +
                                   0.08 * cos(4.0 * CV_PI * i / steps);
    }
}

void Specgram::hamming(int steps) {
    for (int i = 0; i <= steps; i++) {
        _window->at<float>(0, i) = 0.54 - 0.46 * cos((2.0 * CV_PI * i) / steps);
    }
}

void Specgram::bartlett(int steps) {
    for (int i = 0; i <= steps; i++) {
        _window->at<float>(0, i) = 1.0 - 2.0 * fabs(i - steps / 2.0) / steps;
    }
}

void Specgram::createWindow(int windowType) {
    typedef void(Specgram::*winFunc)(int);
    winFunc funcs[] = {&Specgram::none, &Specgram::hann,
                       &Specgram::blackman, &Specgram::hamming,
                       &Specgram::bartlett};
    assert(windowType >= 0);
    if (windowType >= (int) (sizeof(funcs) / sizeof(funcs[0]))) {
        throw std::runtime_error("Unsupported window function");
    }
    int steps = _windowSize - 1;
    assert(steps > 0);
    (this->*(funcs[windowType]))(steps);
}

void Specgram::applyWindow(Mat& signal) {
    if (_window == 0) {
        return;
    }
    for (int i = 0; i < signal.rows; i++) {
        signal.row(i) = signal.row(i).mul((*_window));
    }
}

int Specgram::stridedSignal(shared_ptr<RawMedia> raw) {
    // read from raw in strided windows of length `_windowSize`
    // with at every `_stride` samples.

    // truncate data in raw if larger than _maxSignalSize
    int numSamples = raw->numSamples();
    if (numSamples > _maxSignalSize) {
        numSamples = _maxSignalSize;
    }

    // assert that there is more than 1 window of data in raw
    assert(numSamples >= _windowSize);

    // count is the number of windows to capture
    int count = ((numSamples - _windowSize) / _stride) + 1;

    // ensure the numver of windows will fit in output buffer of
    // width `_width`
    assert(count <= _width);

    char* src = raw->getBuf(0);
    char* dst = _buf;

    int windowSizeInBytes = _windowSize * raw->bytesPerSample();
    int strideInBytes = _stride * raw->bytesPerSample();

    assert(count * strideInBytes <= numSamples * raw->bytesPerSample());
    assert(count * windowSizeInBytes <= _bufSize);

    for (int i = 0; i < count; i++) {
        memcpy(dst, src, windowSizeInBytes);
        dst += windowSizeInBytes;
        src += strideInBytes;
    }

    return count;
}

double Specgram::hzToMel(double freqInHz) {
    return 2595 * std::log10(1 + freqInHz/700.0);
}

double Specgram::melToHz(double freqInMels) {
    return 700 * (std::pow(10, freqInMels/2595.0)-1);
}

vector<double> Specgram::linspace(double a, double b, int n) {
    vector<double> interval;
    double delta = (b-a)/(n-1);
    while (a <= b) {
        interval.push_back(a);
        a += delta;
    }
    interval.push_back(a);
    return interval;
}

Mat Specgram::getFilterbank(int filts, int ffts, double samplingRate) {
    double minFreq = 0.0;
    double maxFreq = samplingRate / 2.0;
    double minMelFreq = hzToMel(minFreq);
    double maxMelFreq = hzToMel(maxFreq);
    vector<double> melInterval = linspace(minMelFreq, maxMelFreq, filts + 2);
    vector<int> bins;
    for (int k=0; k<filts+2; ++k) {
        bins.push_back(std::floor((1+ffts)*melToHz(melInterval[k])/samplingRate));
    }

    Mat fbank = Mat::zeros(filts, 1 + ffts / 2, CV_32F);
    for (int j=0; j<filts; ++j) {
        for (int i=bins[j]; i<bins[j+1]; ++i) {
            fbank.at<float>(j, i) = (i - bins[j]) / (1.0*(bins[j + 1] - bins[j]));
        }
        for (int i=bins[j+1]; i<bins[j+2]; ++i) {
            fbank.at<float>(j, i) = (bins[j+2]-i) / (1.0*(bins[j + 2] - bins[j+1]));
        }
    }
    return fbank;
}

void Specgram::extractFeatures(Mat& spectrogram, Mat& features) {
    Mat powspec = spectrogram.mul(spectrogram);
    powspec *= 1.0 / _windowSize;
    Mat cepsgram = powspec*_fbank;
    log(cepsgram, cepsgram);
    if (_feature == MFSC) {
        features = cepsgram;
        return;
    }
    int pad_cols = cepsgram.cols;
    int pad_rows = cepsgram.rows;
    if (cepsgram.cols % 2 != 0) {
        pad_cols = 1 + cepsgram.cols;
    }
    if (cepsgram.rows % 2 != 0) {
        pad_rows = 1 + cepsgram.rows;
    }
    Mat padcepsgram = Mat::zeros(pad_rows, pad_cols, CV_32F);
    cepsgram.copyTo(padcepsgram(Range(0, cepsgram.rows), Range(0, cepsgram.cols)));
    dct(padcepsgram, padcepsgram, cv::DFT_ROWS);
    cepsgram = padcepsgram(Range(0, cepsgram.rows), Range(0, cepsgram.cols));
    features = cepsgram(Range::all(), Range(0, _numCepstra));
}
