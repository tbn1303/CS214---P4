Name: Thai Nguyen
NetID: tbn22

Name: Zachary Adam
NetID: zea6

Project Description:



Test Plan:
-------------------------------------------------------------------------------------------------------------------


Test Basic Connection of Two Players:

Testing programs ability to host at least two players example run:

Running our host porgram with example port like:
./nimd 8080 

and then run two separate test clients in two separate terminals:

Test Client terminal 1:
./testc localhost 8080 Guest1

Test Client terminal 2:
./testc localhost 8080 Guest2

and then in the terminal where our host server is that we are testing should receive both players. 
The host will return some messages like:

Server listening on port 5040
Started game pid=.......: Guest1 vs Guest2

-------------------------------------------------------------------------------------------------------------------


Test Error Condition for Invalid:

Test programs that cannot read the message or message is not valid (not match the format). 
When either players send an invalid message, the host will send a message of invalid to both player and close the connection on the player that send invalid message.

-------------------------------------------------------------------------------------------------------------------


Test Error Condition for Long Name:

Program that test for long name.
If player send the right message with the right format, however, the name is too long, the connection will be closed and the player with the long name has to reconnect.

-------------------------------------------------------------------------------------------------------------------


Test Error for Already Playing:

This program will check if the player that try to connect to the server has the same name with the player that already in the server and is playing.
It will check for that name and close connection if the name already created and close the connection.

-------------------------------------------------------------------------------------------------------------------


Test Error for Already Open:



-------------------------------------------------------------------------------------------------------------------


Test Error for Not Playing:



-------------------------------------------------------------------------------------------------------------------


Test Error for Impatient: