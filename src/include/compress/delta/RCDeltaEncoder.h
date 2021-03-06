/********************************************************************************
 *  版权所有(C)2008,2009,2010，好压软件工作室，保留所有权利。                   *
 ********************************************************************************
 *  作者    : HaoZip                                                            *
 *  版本    : 1.7                                                               *
 *  联系方式: haozip@gmail.com                                                  *
 *  官方网站: www.haozip.com                                                    *
 ********************************************************************************/

#ifndef __RCDeltaEncoder_h_
#define __RCDeltaEncoder_h_ 1

#include "interface/ICoder.h"
#include "interface/IUnknownImpl.h"
#include "RCDelta.h"

BEGIN_NAMESPACE_RCZIP

/** Delta 编码器
*/
class RCDeltaEncoder:
    public RCDelta,
    public IUnknownImpl3<ICompressFilter,
                         ICompressSetCoderProperties,
                         ICompressWriteCoderProperties>
{
public:
    
    /** 初始化
    @return 成功返回RC_S_OK,否则返回错误号
    */
    virtual HResult Init() ;

    /** 过滤数据
    @param [in,out] data 数据缓冲区
    @param [in] size 数据长度
    @return 实际处理数据的长度
    */
    virtual uint32_t Filter(byte_t* data, uint32_t size) ;

    /** 设置压缩编码属性
    @param [in] propertyArray 压缩编码属性列表
    @return 成功返回RC_S_OK,否则返回错误号
    */
    virtual HResult SetCoderProperties(const RCPropertyIDPairArray& propertyArray) ;

    /** 将压缩编码属性写入输出流
    @param [in] outStream 输出流接口指针
    @return 成功返回RC_S_OK,否则返回错误号
    */
    virtual HResult WriteCoderProperties(ISequentialOutStream* outStream) ;
};

END_NAMESPACE_RCZIP

#endif //__RCDeltaEncoder_h_
