#ifndef S3MANAGERKIT_USERENTITY_H
#define S3MANAGERKIT_USERENTITY_H

#include "DbStorage.h"
#include "Util/util.h"
#include <string>

namespace managerkit {

struct UserEntity {
    std::string userId;
    Optional<std::string> userName;
};

DECLARE_ENTITY(UserEntity, "user_entities", 
    { "userId" }, 
    &UserEntity::userId, "userId", 
    &UserEntity::userName, "userName"
)

class UserEntityRepository : public SqliteRepository<UserEntity> {
public:
    UserEntityRepository() : SqliteRepository<UserEntity>(Database::kEdgeStorageControllerDb) {}

protected:
};

class UserEntityImp : public UserEntityRepository {
public:
    using Ptr = std::shared_ptr<UserEntityImp>;
    UserEntityImp() : UserEntityRepository() {}

    void add(UserEntity &entity) {
        auto entities = findById(entity.userId);   
        if (entities.size() == 0){
            save(entity, true);
        }
    }

    void update(UserEntity &entity) { 
        updateById(entity); 
    }

    void remove(const std::string &userId) {
        UserEntity entity;
        entity.userId = userId;
        removeById(entity);
    }
    
    std::vector<UserEntity> findById(const std::string &id) {
        UserEntity entity_search;
        entity_search.userId = id;
        return UserEntityRepository::findById(entity_search);
    }
}; 

} // namespace managerkit

#endif // S3MANAGERKIT_USERENTITY_H
