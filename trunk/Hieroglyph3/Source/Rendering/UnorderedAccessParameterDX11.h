//--------------------------------------------------------------------------------
// This file is a portion of the Hieroglyph 3 Rendering Engine.  It is distributed
// under the MIT License, available in the root of this distribution and 
// at the following URL:
//
// http://www.opensource.org/licenses/mit-license.php
//
// Copyright (c) 2003-2010 Jason Zink 
//--------------------------------------------------------------------------------

//--------------------------------------------------------------------------------
// UnorderedAccessParameterDX11
//
//--------------------------------------------------------------------------------
#include "RenderParameterDX11.h"
//--------------------------------------------------------------------------------
#ifndef UnorderedAccessParameterDX11_h
#define UnorderedAccessParameterDX11_h
//--------------------------------------------------------------------------------
namespace Glyph3
{
	struct UAVParameterData
	{
		int				m_iUnorderedAccessView;
		unsigned int	m_iInitialCount;
	};

	class UnorderedAccessParameterDX11 : public RenderParameterDX11
	{
	public:
		UnorderedAccessParameterDX11();
		UnorderedAccessParameterDX11( UnorderedAccessParameterDX11& copy );
		virtual ~UnorderedAccessParameterDX11();

		virtual void SetParameterData( void* pData );
		virtual ParameterType GetParameterType();
		int GetIndex();
		unsigned int GetInitialCount();

		void UpdateValue( RenderParameterDX11* pParameter );

	protected:
		UAVParameterData m_ParameterData;
	};
};
//--------------------------------------------------------------------------------
#endif // UnorderedAccessParameterDX11_h
//--------------------------------------------------------------------------------

