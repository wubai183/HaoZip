/********************************************************************************
 *  ��Ȩ����(C)2008,2009,2010����ѹ���������ң���������Ȩ����                   *
 ********************************************************************************
 *  ����    : HaoZip                                                            *
 *  �汾    : 1.7                                                               *
 *  ��ϵ��ʽ: haozip@gmail.com                                                  *
 *  �ٷ���վ: www.haozip.com                                                    *
 ********************************************************************************/

#ifndef __RCBranch86_h_
#define __RCBranch86_h_ 1

#include "base/RCDefs.h"

BEGIN_NAMESPACE_RCZIP

/** x86��֧
*/
class RCBranch86
{
public:

    /**
    */
    uint32_t m_prevMask ;
    
    /** x86��ʼ��
    */
    void x86Init() ;
};

END_NAMESPACE_RCZIP

#endif //__RCBranch86_h_