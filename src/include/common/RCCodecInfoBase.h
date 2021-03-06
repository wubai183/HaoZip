/********************************************************************************
 *  版权所有(C)2008,2009,2010，好压软件工作室，保留所有权利。                   *
 ********************************************************************************
 *  作者    : HaoZip                                                            *
 *  版本    : 1.7                                                               *
 *  联系方式: haozip@gmail.com                                                  *
 *  官方网站: www.haozip.com                                                    *
 ********************************************************************************/

#ifndef __RCCodecInfoBase_h_
#define __RCCodecInfoBase_h_ 1

#include "interface/ICodecInfo.h"
#include "base/RCRefCounted.h"

BEGIN_NAMESPACE_RCZIP

/** 编解码公用基类，实现引用计数功能
*/
class RCCodecInfoBase:
    public ICodecInfo,
    public RCRefCounted
{
public:

    /** 默认构造函数
    */
    RCCodecInfoBase() ;
    
    /** 默认析构函数
    */
    virtual ~RCCodecInfoBase() ;

public:
    
    /** 增加引用计数
    */
    virtual void AddRef(void) ;
    
    /** 减少引用计数
    */
    virtual void Release() ;
};

END_NAMESPACE_RCZIP

#endif //__RCCodecInfoBase_h_
