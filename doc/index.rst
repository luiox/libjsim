libjsim 设计规格
================

跨模块设计契约的权威文本。微观契约（参数、返回值、失败模式、不变量）在代码 ``///`` 注释里（Doxygen 层），本目录只管「为什么这样设计」与「契约是什么」。分层规则见 ``CONTRIBUTING.md`` 的「文档规范」一节。

代码注释以 ``doc:<label>`` 形式指向本目录的具体小节；``check_refs.py`` 校验这些指向不会腐烂。下列文档的标题即为该文档的根 label。

.. toctree::
   :maxdepth: 2
   :caption: 设计与契约

   design
   ext-jbc
   ext-jni
