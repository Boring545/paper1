// paper1.cpp: 定义应用程序的入口点。
//

#include "paper1.h"
#include "frame_packing.h"
using namespace std;

int main()
{
	cout << "Hello CMake." << endl;
	
	//初始化消息集合
	cfd::CanfdUtils::generate_msg_info_set(cfd::message_info_vec, 100);
	//cfd::CanfdUtils::print_message(cfd::message_info_vec,true);

	cfd::PackingScheme ps1(cfd::message_info_vec);
	cfd::CanfdUtils::print_frame(ps1.frame_set);
	return 0;
}
