#include <array>
#include <cmath>
#include <functional>

class DSPKernel {
public:
	virtual void Begin(float sampleRate) = 0;
	virtual float Process(float input) = 0;
	virtual void End( ) = 0;
};

template <int STAGES> class DSPChannel : public DSPKernel {
	std::array<DSPKernel*, STAGES> pipeline;
	DSPKernel*  ref(DSPKernel& krn) { return &krn; }
public:
	template <typename... KERNELS> DSPChannel(KERNELS&... krn) : pipeline{ref(krn)...} { }
	void Begin(float sampleRate) { for (auto krn : pipeline) krn->Begin(sampleRate); }
	void End() { for (auto krn : pipeline) krn->End(); }
	float Process(float sig) { for (auto krn : pipeline) sig = krn->Process(sig); return sig; }
};

template <typename DSP1, typename DSP2, typename OP> class DSPOp {
	DSP1& dsp1; DSP2& dsp2;
public:
	DSPOp(DSP1& dsp1, DSP2& dsp2) :dsp1(dsp1), dsp2(dsp2) { }
	virtual float Process(float in) { OP op; return op(dsp1.Process(in), dsp2.Process(in)); }
	virtual void Begin(float sampleRate) { dsp1.Begin(sampleRate); dsp2.Begin(sampleRate); }
	virtual void End( ) { dsp1.End( ); dsp2.End( ); }
};

#define AUTO_EXPR(expr) ->decltype(expr) { return expr; }

template <typename... KERNELS> static auto Channel(KERNELS&... kernels) AUTO_EXPR(DSPChannel<sizeof...(KERNELS)>(kernels...))
template <int A, int B> static auto operator+(DSPChannel<A> a, DSPChannel<B> b) AUTO_EXPR(DSPOp<DSPChannel<A>,DSPChannel<B>,std::plus<float>>(a, b))
template <int A, int B> static auto operator-(DSPChannel<A> a, DSPChannel<B> b) AUTO_EXPR(DSPOp<DSPChannel<A>,DSPChannel<B>,std::minus<float>>(a, b))
template <int A, int B> static auto operator*(DSPChannel<A> a, DSPChannel<B> b) AUTO_EXPR(DSPOp<DSPChannel<A>,DSPChannel<B>,std::multiplies<float>>(a, b))

void Play(DSPKernel&, int numSamples);

class SinOsc {
	float phase;
	float inc;
public:
	SinOsc(float f) {
		freq = f;
	}

	void Begin(float sampleRate) {
		inc = M_PI * 2 * freq / sampleRate;
	}

	float Process(float in) {

	}
};

int main( ) {

}
