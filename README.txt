Name: Thai Nguyen
NetID: tbn22

Name: Zachary Adam
NetID: zea6

Project Description:



Test Plan:
--------------------------------------------------------------------------------------------------------------------------


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

--------------------------------------------------------------------------------------------------------------------------

Test Error Condition for Invalid:



--------------------------------------------------------------------------------------------------------------------------


Test Error Condition for Long Name:



--------------------------------------------------------------------------------------------------------------------------


Test Error for Already Playing:



--------------------------------------------------------------------------------------------------------------------------


Test Error for Already Open:



--------------------------------------------------------------------------------------------------------------------------


Test Error for Not Playing:



--------------------------------------------------------------------------------------------------------------------------


Test Error for Impatient: