#pragma once

#include <iostream>  // FIXME, remove as this is only here for debug output
#include <vector>
#include <array>
#include <amp.h>
#include <bolt/functional.h>

namespace bolt {

	const int scanMultiCpuThreshold	= 4; // FIXME, artificially low to force use of GPU
	const int scanGpuThreshold		= 8; // FIXME, artificially low to force use of GPU

	//	Work routine for inclusive_scan that contains a compile time constant size
	template< typename InputType, typename OutputType, size_t numElements, typename BinaryFunction > 
	typename std::_Array_iterator< OutputType, numElements >
		inclusive_scan( const concurrency::accelerator_view& av, const std::_Array_iterator< InputType, numElements >& first, const std::_Array_iterator< InputType, numElements >& last, 
		std::_Array_iterator< OutputType, numElements >& result, BinaryFunction binary_op )
	{
		if( numElements < scanMultiCpuThreshold )
		{
			//	Serial CPU implementation
			return std::partial_sum( first, last, result, binary_op);
		} 
		else if( numElements < scanGpuThreshold )
		{
			//	This should be implemented in TBB as tbb::parallel_scan( range, body )
			//	Does not appear to have an implementation in PPL
			//	TODO: Bring in the dependency to TBB and replace this STD call
			return std::partial_sum( first, last, result, binary_op);
		}
		else
		{
			// FIXME - determine from HSA Runtime 
			// - based on est of how many threads needed to hide memory latency.
			static const unsigned int waveSize  = 64; // FIXME, read from device attributes.
			static_assert( (waveSize & (waveSize-1)) == 0, "Scan depends on wavefronts being a power of 2" );
			
			//	AMP code can not read size_t as input, need to cast to int
			//	Note: It would be nice to have 'constexpr' here, then we could use tileSize as the extent dimension
			unsigned int tileSize = std::min( static_cast< unsigned int >( numElements ), waveSize );

			//int computeUnits		= 10; // FIXME - determine from HSA Runtime
			//int wgPerComputeUnit	=  6;
			unsigned int sizeDeviceBuff = numElements;
			size_t modWaveFront = (numElements & (waveSize-1));
			if( modWaveFront )
			{
				sizeDeviceBuff &= ~modWaveFront;
				sizeDeviceBuff += waveSize;
			}
			unsigned int numWorkGroups = sizeDeviceBuff / waveSize;
			unsigned int sizeScanBuff = numWorkGroups;
			modWaveFront = (sizeScanBuff & (waveSize-1));
			if( modWaveFront )
			{
				sizeScanBuff &= ~modWaveFront;
				sizeScanBuff += waveSize;
			}

			//	Wrap our input data in an array_view, and mark it const so data is not read back from device
			concurrency::array_view< const InputType > hostInput( static_cast< int >( numElements ), &first[ 0 ] );

			//	Wrap our output data in an array_view, and discard input data so it is not transferred to device
			concurrency::array< InputType > deviceInput( sizeDeviceBuff, av );
			hostInput.copy_to( deviceInput.section( concurrency::extent< 1 >( numElements ) ) );

			concurrency::array< OutputType > deviceOutput( sizeDeviceBuff, av );
			concurrency::array< OutputType > scanBuffer( sizeScanBuff, av );

			//	Loop to calculate the inclusive scan of each individual tile, and output the block sums of every tile
			concurrency::parallel_for_each( av, deviceOutput.extent.tile< waveSize >(), [&deviceOutput, &deviceInput, &scanBuffer, tileSize]( concurrency::tiled_index< waveSize > idx ) restrict(amp)
			{
				tile_static InputType LDS[ waveSize + ( waveSize / 2 ) ];

				int localID		= idx.local[ 0 ];
				int globalID	= idx.global[ 0 ];

				//	Initialize the padding to 0, for when the scan algorithm looks left.  
				//	Then bump the LDS pointer past the extra padding.
				LDS[ localID ] = 0;
				InputType* pLDS = LDS + ( waveSize / 2 );

				InputType val = deviceInput[ globalID ];
				pLDS[ localID ] = val;

				//	This loop essentially computes a scan within a tile, read from global memory.  No communication with other tiles yet.
				InputType sum = val;
				for( unsigned int offset = 1; offset < tileSize; offset *= 2 )
				{
					InputType y = pLDS[ localID - offset ];
					sum += y;
					pLDS[ localID ] = sum;
				}

				//	Write out the values of the per-tile scan
				deviceOutput[ globalID ] = sum;

				//	Take the very last thread in a tile, and save its value into a buffer for further processing
				if( localID == (waveSize-1) )
				{
					scanBuffer[ idx.tile[ 0 ] ] = pLDS[ localID ];
				}

				////TODO: global_memory_fence( barrier )
			} );

			std::vector< OutputType > scanData( sizeScanBuff );
			scanData = scanBuffer;

			//	Loop to calculate the exclusive scan of the block sums
			tileSize = static_cast< unsigned int >( std::min( numWorkGroups, waveSize ) );
			concurrency::parallel_for_each( av, scanBuffer.extent.tile< waveSize >(), [&scanBuffer, tileSize]( concurrency::tiled_index< waveSize > idx ) restrict(amp)
			{
				tile_static OutputType LDS[ waveSize + ( waveSize / 2 ) ];

				int localID		= idx.local[ 0 ];
				int globalID	= idx.global[ 0 ];

				//	Initialize the padding to 0, for when the scan algorithm looks left.  
				//	Then bump the LDS pointer past the extra padding.
				LDS[ localID ] = 0;
				OutputType* pLDS = LDS + ( waveSize / 2 );

				OutputType val = scanBuffer[ globalID ];
				pLDS[ localID ] = val;

				//	This loop essentially computes an exclusive scan within a tile, writing 0 out for first element.
				OutputType sum = val;
				for( unsigned int offset = 1; offset < tileSize; offset *= 2 )
				{
					OutputType y = pLDS[ localID - offset ];
					sum += y;
					pLDS[ localID ] = sum;
				}

				//	Write out the values of the per-tile scan
				scanBuffer[ globalID ] = sum - val;
			} );
			scanData = scanBuffer;

			//	Loop through the entire output array and add the exclusive scan back into the output array
			concurrency::parallel_for_each( av, deviceOutput.extent.tile< waveSize >(), [&deviceOutput, &scanBuffer]( concurrency::tiled_index< waveSize > idx ) restrict(amp)
			{
				int globalID	= idx.global[ 0 ];
				int tileID		= idx.tile[ 0 ];

				//	Even though each wavefront threads access the same bank, it's the same location so there should not be bank conflicts
				OutputType val = scanBuffer[ tileID ];

				//	Write out the values of the per-tile scan
				deviceOutput[ globalID ] += val;
			} );

			concurrency::array_view< OutputType > hostOutput( static_cast< int >( numElements ), &result[ 0 ] );
			hostOutput.discard_data( );

			deviceOutput.section( Concurrency::extent< 1 >( numElements ) ).copy_to( hostOutput );

		};

		return result + numElements;
	};

	//	Work routine for inclusive_scan that contains a compile time constant size
	template< typename InputType, typename OutputType, size_t numElements, typename BinaryFunction > 
	typename std::_Array_iterator< OutputType, numElements >
		inclusive_scan( const concurrency::accelerator_view& av, const InputType (&first)[numElements], const InputType (&last)[numElements], 
		OutputType (&result)[numElements], BinaryFunction binary_op )
	{
		if( numElements < scanMultiCpuThreshold )
		{
			//	Serial CPU implementation
			return std::partial_sum( first, last, result, binary_op);
		} 
		else if( numElements < scanGpuThreshold )
		{
			//	This should be implemented in TBB as tbb::parallel_scan( range, body )
			//	Does not appear to have an implementation in PPL
			//	TODO: Bring in the dependency to TBB and replace this STD call
			return std::partial_sum( first, last, result, binary_op);
		}
		else
		{
			// FIXME - determine from HSA Runtime 
			// - based on est of how many threads needed to hide memory latency.
			static const size_t waveSize  = 64; // FIXME, read from device attributes.
			
			//	AMP code can not read size_t as input, need to cast to int
			const unsigned int tileSize = static_cast< unsigned int >( std::min( numElements, waveSize ) );

			int computeUnits		= 10; // FIXME - determine from HSA Runtime
			int wgPerComputeUnit	=  6; 
			int resultCnt			= computeUnits * wgPerComputeUnit;

			//	Wrap our input data in an array_view, and mark it const so data is not read back from device
			concurrency::array_view< const InputType > avInput( static_cast< int >( numElements ), &first[ 0 ] );

			//	Wrap our output data in an array_view, and discard input data so it is not transferred to device
			concurrency::array_view< OutputType > avOutput( static_cast< int >( numElements ), &result[ 0 ] );
			avOutput.discard_data( );

			concurrency::parallel_for_each( av, avOutput.extent.tile< numElements >(), [avOutput, avInput, tileSize](concurrency::tiled_index< numElements > idx) restrict(amp)
//			concurrency::parallel_for_each( av, avOutput.extent, [=](concurrency::index< 1 > idx) restrict(amp)
			{
				tile_static InputType LDS[ numElements + (numElements / 2) ];

				int tId = idx.global[ 0 ];

				LDS[ tId ] = 0;
//				T* pLDS = LDS + (waveSize / 2) + tId;
				InputType* pLDS = LDS + (numElements / 2);

				InputType val = avInput[ tId ];
				pLDS[ tId ] = val;

				InputType sum = val;
				for( unsigned int offset = 1; offset < tileSize; offset *= 2 )
				{
					InputType y = pLDS[ tId - offset ];
					sum += y;
					pLDS[ tId ] = sum;
				}

				avOutput[ tId ] = sum;
			} );
		};

			return result + numElements;
	};

	template<typename InputIterator, typename OutputIterator, typename BinaryFunction> 
	OutputIterator inclusive_scan( concurrency::accelerator_view av, InputIterator first, InputIterator last, 
		OutputIterator result, BinaryFunction binary_op )
	{
		typedef std::iterator_traits< InputIterator >::value_type T;

		size_t numElements = std::distance( first, last );

		if( numElements < scanMultiCpuThreshold )
		{
			//	Serial CPU implementation
			return std::partial_sum( first, last, result, binary_op);
		} 
		else if( numElements < scanGpuThreshold )
		{
			//	This should be implemented in TBB as tbb::parallel_scan( range, body )
			//	Does not appear to have an implementation in PPL
			//	TODO: Bring in the dependency to TBB and replace this STD call
			return std::partial_sum( first, last, result, binary_op);

			return result + numElements;
		}
		else
		{
			// FIXME - determine from HSA Runtime 
			// - based on est of how many threads needed to hide memory latency.
			static const size_t waveSize  = 64; // FIXME, read from device attributes.
			
			//	AMP code can not read size_t as input, need to cast to int
			const unsigned int tileSize = static_cast< unsigned int>( std::min( numElements, waveSize ) );

			int computeUnits		= 10; // FIXME - determine from HSA Runtime
			int wgPerComputeUnit	=  6; 
			int resultCnt			= computeUnits * wgPerComputeUnit;

			//	Wrap our input data in an array_view, and mark it const so data is not read back from device
			concurrency::array_view< const T > avInput( static_cast< int >( numElements ), &first[ 0 ] );

			//	Wrap our output data in an array_view, and discard input data so it is not transferred to device
			concurrency::array_view< T > avOutput( static_cast< int >( numElements ), &result[ 0 ] );
			avOutput.discard_data( );

			//	This is a very basic, basic scan implementation (non-optimized).  There is no synchronization, it works if we have 
			//	32 <= x <= 64 threads.  Don't know why less than 32 theads returns garbage.

			concurrency::parallel_for_each( av, avOutput.extent.tile< waveSize >(), [avOutput, avInput, tileSize](concurrency::tiled_index< waveSize > idx) restrict(amp)
//			concurrency::parallel_for_each( av, avOutput.extent, [=](concurrency::index< 1 > idx) restrict(amp)
			{
				tile_static T LDS[ waveSize + (waveSize / 2) ];

				int tId = idx.global[ 0 ];
//				int tId = idx[ 0 ];

				LDS[ tId ] = 0;
//				T* pLDS = LDS + (waveSize / 2) + tId;
				T* pLDS = LDS + (tileSize / 2);

				T val = avInput[ tId ];
				pLDS[ tId ] = val;

				T sum = val;
				for( unsigned int offset = 1; offset < tileSize; offset *= 2 )
				{
					T y = pLDS[ tId - offset ];
					sum += y;
					pLDS[ tId ] = sum;
				}

				avOutput[ tId ] = sum;
			} );
		};

			return result + numElements;
	};

	/*
	* This version of inclusive_scan defaults to disallow the use of iterators, unless a specialization exists below
	*/
	template< typename InputIterator, typename OutputIterator, typename BinaryFunction >
	OutputIterator inclusive_scan( InputIterator begin, InputIterator end, OutputIterator result,
		BinaryFunction binary_op, std::input_iterator_tag )
	{
		return std::iterator_traits< OutputIterator >::value_type( );
	};

	/*
	* Partial specialization of inclusive_scan which allows the use of naked pointer types
	*/
	template< typename T, typename BinaryFunction >
	T* inclusive_scan( T* begin, typename T* end, 
		BinaryFunction binary_op, std::random_access_iterator_tag )
	{
		return inclusive_scan( concurrency::accelerator().default_view, begin, end, binary_op);
	};

	/*
	* Partial specialization of inclusive_scan which allows the use of constant naked pointer types
	*/
	template< typename T, typename BinaryFunction >
	const T* inclusive_scan( const T* begin, const T* end, 
		BinaryFunction binary_op, std::random_access_iterator_tag )
	{
		return inclusive_scan( concurrency::accelerator().default_view, begin, end, binary_op);
	};

	/*
	* Partial specialization of inclusive_scan which allows the use of std::vector< T >::iterator types
	*/
	template< typename T, typename BinaryFunction >
	typename std::vector< T >::iterator
		inclusive_scan(typename std::vector< T >::iterator begin, typename std::vector< T >::iterator end, 
		BinaryFunction binary_op, std::random_access_iterator_tag )
	{
		return inclusive_scan( concurrency::accelerator().default_view, begin, end, binary_op );
	};

	/*
	* Partial specialization of inclusive_scan which allows the use of std::vector< T >::const_iterator types
	*/
	template< typename T, typename BinaryFunction >
	typename std::vector< T >::const_iterator
		inclusive_scan(typename std::vector< T >::const_iterator begin, typename std::vector< T >::const_iterator end, 
		BinaryFunction binary_op, std::random_access_iterator_tag )
	{
		return inclusive_scan( concurrency::accelerator().default_view, begin, end, binary_op );
	};

	/*
	* Partial specialization of inclusive_scan which disallows the use of std::vector< bool >::iterator types
	*/
	template< typename BinaryFunction >
	typename std::vector< bool >::iterator
		inclusive_scan(typename std::vector< bool >::iterator begin, typename std::vector< bool >::iterator end, 
		BinaryFunction binary_op, std::random_access_iterator_tag )
	{
		return std::iterator_traits<typename std::vector< bool >::iterator >::value_type( );
	};

	/*
	* Partial specialization of inclusive_scan which disallows the use of std::vector< bool >::const_iterator types
	*/
	template< typename BinaryFunction >
	typename std::vector< bool >::const_iterator
		inclusive_scan(typename std::vector< bool >::const_iterator begin, typename std::vector< bool >::const_iterator end, 
		BinaryFunction binary_op, std::random_access_iterator_tag )
	{
		return std::iterator_traits< typename std::vector< bool >::const_iterator >::value_type( );
	};

	/*
	* Partial specialization of inclusive_scan which allows the use of std::array< T, N >::iterator types
	*/
	template< typename I, typename O, size_t N, typename BinaryFunction >
	typename std::_Array_iterator< O, N >
		inclusive_scan(const typename std::_Array_iterator< I, N >& first, const typename std::_Array_iterator< I, N >& last, typename std::_Array_iterator< O, N >& result,
		BinaryFunction binary_op, std::random_access_iterator_tag )
	{
		return inclusive_scan( concurrency::accelerator().default_view, first, last, result, binary_op );
	};

	/*
	* Partial specialization of inclusive_scan which allows the use of std::array< T, N >::const_iterator types
	*/
	template< typename I, typename O, size_t N, typename BinaryFunction >
	typename std::_Array_const_iterator< O, N >
		inclusive_scan(const typename std::_Array_const_iterator< I, N >& first, const typename std::_Array_const_iterator< I, N >& last, typename std::_Array_const_iterator< O, N >& result,
		BinaryFunction binary_op, std::random_access_iterator_tag )
	{
		return inclusive_scan( concurrency::accelerator().default_view, first, last, result, binary_op );
	};

	/*
	* This version of inclusive_scan uses default accelerator
	*/
	template< typename InputIterator, typename OutputIterator, typename BinaryFunction > 
	OutputIterator inclusive_scan(InputIterator first, InputIterator last, OutputIterator result, BinaryFunction binary_op )
	{

		return inclusive_scan( first, last, result, binary_op, std::iterator_traits< InputIterator >::iterator_category( ) );
	};

	/*
	* This version of inclusive_scan uses a default init value of 0 and plus<> as default argument.
	*/
	template< typename InputIterator, typename OutputIterator >
	OutputIterator inclusive_scan( const InputIterator& first, const InputIterator& last, OutputIterator& result )
	{
		typedef std::iterator_traits<InputIterator>::value_type T;

		return inclusive_scan( first, last, result, bolt::plus< T >( ), std::iterator_traits< InputIterator >::iterator_category( ) );
	};


	// still need more versions that take accelerator as first argument.


}; // end namespace bolt