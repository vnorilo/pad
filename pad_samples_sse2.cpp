#include "pad_samples.h"
#include "pad_samples_sse2.h"
#include <emmintrin.h>

namespace PAD{
	namespace Converter{
		template <> void Transpose<>(SampleVector<float,4> *v)
		{
			__m128i t0 = _mm_castps_si128(_mm_unpacklo_ps(v[0].data,v[1].data));
			__m128i t1 = _mm_castps_si128(_mm_unpacklo_ps(v[2].data,v[3].data));
			__m128i t2 = _mm_castps_si128(_mm_unpackhi_ps(v[0].data,v[1].data));
			__m128i t3 = _mm_castps_si128(_mm_unpackhi_ps(v[2].data,v[3].data));

			v[0] = _mm_castsi128_ps(_mm_unpacklo_epi64(t0,t1));
			v[1] = _mm_castsi128_ps(_mm_unpackhi_epi64(t0,t1));
			v[2] = _mm_castsi128_ps(_mm_unpacklo_epi64(t2,t3));
			v[3] = _mm_castsi128_ps(_mm_unpackhi_epi64(t2,t3));
		}
	}
}
