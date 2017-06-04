after git clone 


git submodule init
git submodule update

download boost , mysqlcpp connector , tbb seperately and link this source..




1) Game type sessions
 - create a session of game type
 - join nodes to this session , it can be controlled by who can add to that session
 - every message sent from any node , is sent to master_node.
 - every message sent by master node is broadcasted to the session nodes.
 
 - notify clients when master connects/disconnects.
 - 
 
 
 
 
2) TODO:
- node can set a specific msg to send when it disconnects.( to a dest_id , dest_session_id.. etc)
- node can join a session temporarily if it wants. 
- when untracking connection , any game sessions should be notified ?
 
 
 
 